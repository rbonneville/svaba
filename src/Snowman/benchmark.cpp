#include "benchmark.h"

#include <getopt.h>
#include <iostream>
#include <string>
#include <sstream>
#include <fstream>
#include <cstdlib>
#include <algorithm>

#include "vcf.h"
#include "SnowTools/SnowToolsCommon.h"
#include "SnowTools/GenomicRegion.h"
#include "SnowTools/SnowUtils.h"
#include "SnowmanAssemblerEngine.h"
#include "KmerFilter.h"
#include "ReadSim.h"
#include "SeqFrag.h"
#include "SimGenome.h"
#include "BamSplitter.h"

#include "SnowTools/BWAWrapper.h"
#include "SnowTools/AlignedContig.h"
#include "SnowTools/Fractions.h"


static std::vector<double> snv_error_rates;
static std::vector<double> del_error_rates;
static std::vector<double> ins_error_rates;
static std::vector<double> coverages;
static std::vector<double> fractions;

static SnowTools::GRC regions;
static SnowTools::Fractions fractions_bed;
static SnowTools::BamWalker bwalker;
static faidx_t * findex;

#define DEFAULT_SNV_RATE 0.01
#define DEFAULT_DEL_RATE 0.05
#define DEFAULT_INS_RATE 0.05
#define DEFAULT_COV 10

namespace opt {

  static std::string refgenome = SnowTools::REFHG19;  
  static int mode = -1;
  static size_t readlen = 101;
  static int num_runs = 100;
  static uint32_t seed = 0;
  static std::string regionFile = "";
  static std::string bam = "";
  static int isize_mean = 250;
  static int isize_sd = 50;

  static int nbreaks = 10;
  static int nindels = 10;

  static std::string string_id = "noid";

  static std::string frac_bed_file;

}

enum { 
  OPT_ASSEMBLY,
  OPT_SIMBREAKS,
  OPT_ISIZE_MEAN,
  OPT_ISIZE_SD,
  OPT_SPLITBAM
};


static const char *BENCHMARK_USAGE_MESSAGE =
"Usage: snowman benchmark\n\n"
"  Description: Various benchmarking tests for Snowman\n"
"\n"
"  General options\n"
"  -v, --verbose                        Select verbosity level (0-4). Default: 1 \n"
"  -G, --reference-genome               Indexed ref genome for BWA-MEM. Default (Broad): /seq/reference/...)\n"
"  -s, --seed                           Seed for the random number generator\n"
"  -A, --string-id                      String to name output files with (e.g. <string-id>_0_01.bam\n"
"  Choose one of the following:\n"     
"      --test-assembly                  Generate single-end reads from small contigs to test assembly/remapping\n"
"      --sim-breaks                     Simulate rearrangements and indels and output paired-end reads\n"
"      --split-bam                      Divide up a BAM file into smaller sub-sampled files, with no read overlaps between files. Preserves read-pairs\n"
"  Shared Options for Test and Simulate:\n"
"  -c, --read-covearge                  Desired coverage. Input as comma-separated to test multiple (test assembly)\n"
"  -b, --bam                            BAM file to train the simulation with\n"
"  -k, --regions                        Regions to simulate breaks or test assembly\n"
"  -e, --snv-error-rate                 The random SNV error rate per base. Input as comma-separated to test multiple (test assembly)\n"
"  -I, --ins-error-rate                 The random insertion error rate per read. Input as comma-separated to test multiple (test assembly)\n"
"  -D, --del-error-rate                 The random deletion error rate per read. Input as comma-separated to test multiple (test assembly)\n"
"  Test Assembly (--test-assembly) Options:\n"
"  -n, --num-runs                       Number of random trials to run\n"
"  Simulate Breaks (--sim-breaks)  Options:\n"
"      --isize-mean                     Desired mean insert size for the simulated reads\n"
"      --isize-sd                       Desired std. dev. forinsert size for the simulated reads\n"
"  -R, --num-rearrangements             Number of rearrangements to simulate\n"
"  -X, --num-indels                     Number of indels to simulate\n"
"  Split Bam (--split-bam)  Options:\n"
"  -f. --fractions                      Fractions to split the bam into\n"
"\n";


static const char* shortopts = "haG:c:n:s:k:b:E:I:D:R:X:A:f:";
static const struct option longopts[] = {
  { "help",                 no_argument, NULL, 'h' },
  { "reference-genome",     required_argument, NULL, 'G' },
  { "string-id",            required_argument, NULL, 'A' },
  { "seed",                 required_argument, NULL, 's' },
  { "num-runs",             required_argument, NULL, 'n' },
  { "regions",              required_argument, NULL, 'k' },
  { "bam",                  required_argument, NULL, 'b' },
  { "read-coverage",        required_argument, NULL, 'c' },
  { "snv-error-rate",       required_argument, NULL, 'E' },
  { "del-error-rate",       required_argument, NULL, 'D' },
  { "ins-error-rate",       required_argument, NULL, 'I' },
  { "num-rearrangements",   required_argument, NULL, 'R' },
  { "fractions",            required_argument, NULL, 'f' },
  { "num-indels",           required_argument, NULL, 'X' },
  { "test-assembly",        no_argument, NULL, OPT_ASSEMBLY},
  { "sim-breaks",           no_argument, NULL, OPT_SIMBREAKS},
  { "split-bam",            no_argument, NULL, OPT_SPLITBAM},
  { NULL, 0, NULL, 0 }
};

void runBenchmark(int argc, char** argv) {

  parseBenchmarkOptions(argc, argv);

  std::cerr << 
    "-----------------------------------------" << std::endl << 
    "--- Running Snowman Benchmarking Test ---" << std::endl <<
    "-----------------------------------------" << std::endl;
  if (opt::mode == OPT_ASSEMBLY)
    std::cerr << "********* RUNNING ASSEMBLY TEST ***********" << std::endl;
  else if (opt::mode == OPT_SIMBREAKS)
    std::cerr << "********* RUNNING SIMULATE BREAKS ***********" << std::endl;
  else if (opt::mode == OPT_SPLITBAM)
    std::cerr << "********* RUNNING SPLIT BAM ***********" << std::endl;

  if (opt::mode == OPT_ASSEMBLY || opt::mode == OPT_SIMBREAKS) {
    std::cerr << "    Error rates:" << std::endl;
    std::cerr << errorRateString(snv_error_rates, "SNV") << std::endl;
    std::cerr << errorRateString(ins_error_rates, "Del") << std::endl;
    std::cerr << errorRateString(del_error_rates, "Ins") << std::endl;
    std::cerr << errorRateString(coverages, "Coverages") << std::endl;
    std::cerr << "    Insert size: " << opt::isize_mean << "(" << opt::isize_sd << ")" << std::endl;
  } else if (opt::mode == OPT_SPLITBAM) {
    std::cerr << errorRateString(fractions, "Fractions") << std::endl;
  }

  // open the BAM
  if (opt::bam.length()) 
    bwalker = SnowTools::BamWalker(opt::bam);
  
  // parse the region file
  if (opt::regionFile.length()) {
    if (SnowTools::read_access_test(opt::regionFile))
      regions.regionFileToGRV(opt::regionFile, 0, bwalker.header());
    // samtools format
    else if (opt::regionFile.find(":") != std::string::npos && opt::regionFile.find("-") != std::string::npos) {
      if (!bwalker.header()) {
	std::cerr << "Error: To parse a samtools style string, need a BAM header. Input bam with -b" << std::endl;
	exit(EXIT_FAILURE);
      }
      regions.add(SnowTools::GenomicRegion(opt::regionFile, bwalker.header()));    
    } else {
      std::cerr << "Can't parse the regions. Input as BED file or Samtools style string (requires BAM with -b to for header info)" << std::endl;
      exit(EXIT_FAILURE);
    }
    if (!regions.size()) {
      std::cerr << "ERROR: Must input a region to run on " << std::endl;
      exit(EXIT_FAILURE);
    }
  }

  // seed the RNG
  if (opt::seed == 0)
    opt::seed = (unsigned)time(NULL);
  srand(opt::seed);
  std::cerr << "   Seed: " << opt::seed << std::endl;

  // read the fractions file
  if (opt::frac_bed_file.length() && opt::mode == OPT_SPLITBAM) {
    fractions_bed.readFromBed(opt::frac_bed_file, bwalker.header());
    //  fractions_bed.readBEDfile(opt::frac_bed_file.length(), 0, bwalker.header());
  }

  // 
  if (opt::mode == OPT_ASSEMBLY)
    assemblyTest();
  else if (opt::mode == OPT_SIMBREAKS)
    genBreaks();
  else if (opt::mode == OPT_SPLITBAM)
    splitBam();
  else 
    std::cerr << "Mode not recognized. Chose from: --test-assembly, --sim-breaks, --split-bam" << std::endl;

}

std::string genBreaks() {

  // train on the input BAM
  std::vector<std::string> quality_scores;
  std::vector<SnowTools::GenomicRegion> v = {
    SnowTools::GenomicRegion(0, 1000000,1001000), 
    SnowTools::GenomicRegion(0, 2000000,2001000),
    SnowTools::GenomicRegion(0, 3000000,3001000),
    SnowTools::GenomicRegion(0, 4000000,4001000),      
    SnowTools::GenomicRegion(0, 5000000,5001000), 
    SnowTools::GenomicRegion(0, 6000000,6001000),
    SnowTools::GenomicRegion(0, 7000000,7001000),
    SnowTools::GenomicRegion(0, 8000000,8001000) 
  };
  
  bwalker.setBamWalkerRegions(v);
  SnowTools::BamRead r; bool dum;
  std::cerr << "...sampling reads to learn quality scores" << std::endl;
  while (bwalker.GetNextRead(r, dum)) {
    quality_scores.push_back(r.Qualities());
  }
  
  std::cerr << "...loading the reference genome" << std::endl;
  findex = fai_load(opt::refgenome.c_str());  // load the reference

  SnowTools::GenomicRegion gg = regions[0];
  std::cerr << "--Generating breaks on: " << gg << std::endl; 
  std::cerr << "--Total number of rearrangement breaks: " << opt::nbreaks << std::endl; 
  std::cerr << "--Total (approx) number of indels: " << opt::nindels << std::endl; 

  SimGenome sg(gg, opt::nbreaks, opt::nindels, findex);
  
  std::string final_seq = sg.getSequence();
  
  ReadSim rs;

  std::ofstream ind;
  ind.open("indels.tsv", std::ios::out);
  for (auto& i : sg.m_indels)
    ind << i << std::endl;
  ind.close();
  
  rs.addAllele(final_seq, 1);

  // sample paired reads
  std::vector<std::string> reads1;
  std::vector<std::string> reads2;
  std::cerr << "Simulating reads at coverage of " << coverages[0] << " del rate " << del_error_rates[0] << 
    " ins rate " << ins_error_rates[0] << " snv-rate " << snv_error_rates[0] << 
    " isize " << opt::isize_mean << "(" << opt::isize_sd << ")" << std::endl;
  rs.samplePairedEndReadsToCoverage(reads1, reads2, coverages[0], snv_error_rates[0], ins_error_rates[0], del_error_rates[0], 
				    opt::readlen, opt::isize_mean, opt::isize_sd);
  assert(reads1.size() == reads2.size());
  
  // write the paired end fastq
  std::ofstream pe1;
  pe1.open("paired_end1.fastq", std::ios::out);
  size_t ccc= 0;
  for (auto& i : reads1)
    pe1 << "@r" << ccc++ << std::endl << i << std::endl << "+\n" << quality_scores[rand() % quality_scores.size()] << std::endl;
  pe1.close();
  
  std::ofstream pe2;
  pe2.open("paired_end2.fastq", std::ios::out);
  ccc= 0;
  for (auto& i : reads2)
    pe2 << "@r" << ccc++ << std::endl << i << std::endl << "+\n" << quality_scores[rand() % quality_scores.size()] << std::endl;
  pe2.close();


  std::ofstream con;
  con.open("connections.tsv", std::ios::out);
  con << sg.printBreaks();
  con.close();

  std::cerr << "Suggest running: " << std::endl;
  std::cerr << "\nbwa mem $REFHG19 paired_end1.fastq paired_end2.fastq > sim.sam && samtools view sim.sam -Sb > tmp.bam && " << 
    "samtools sort -m 4G tmp.bam sim && rm sim.sam tmp.bam && samtools index sim.bam" << std::endl;

  return "";
}

void splitBam() {

  BamSplitter bs(opt::bam, opt::seed);

  // set the regions to split on
  if (regions.size()) 
    bs.setBamWalkerRegions(regions.asGenomicRegionVector());


  if (fractions_bed.size()) {
    bs.fractionateBam(opt::string_id + ".fractioned.bam", fractions_bed);
  } else {

    // set the output bams
    std::vector<std::string> fnames;
    for (auto& i : fractions)
      fnames.push_back(opt::string_id + to_string(i) + "_subsampled.bam");
    
    bs.setWriters(fnames, fractions);
    bs.splitBam();
  }

}

void assemblyTest() {

  SnowTools::GenomicRegion gr(16, 7565720, 7575000); //"chr17:7,569,720-7,592,868");
 
  std::cerr << "...loading the reference genome" << std::endl;
  findex = fai_load(opt::refgenome.c_str());  // load the reference
  std::string local_ref = getRefSequence(gr, findex);

  size_t seqlen = local_ref.length();
  if (seqlen * 2 <= opt::readlen) {
    std::cerr << "**** Read length must be > 2 * sequence length" << std::endl;
    exit(EXIT_FAILURE);
  }

  // make the BWA Wrapper
  std::cerr << "...constructing local_seq index" << std::endl;
  SnowTools::BWAWrapper local_bwa; 
  local_bwa.constructIndex({{"local_ref", local_ref}});

  // align local_seq to itself
  SnowTools::BamReadVector self_align;
  local_bwa.alignSingleSequence(local_ref, "local_ref", self_align, false);

  // write out the index
  local_bwa.writeIndexToFiles("local_ref");
  std::ofstream fa;
  fa.open("local_ref.fa");
  fa << ">local_ref" << std::endl << local_ref << std::endl;
  fa.close();

  std::cout << "coverage\tnumreads\tnumcontigs\tnumfinal\tcontig_coverage\tkmer_corr\terror_rate" << std::endl;
  for (int rep = 0; rep < opt::num_runs; ++rep) {
    std::cerr << "...assembly test. Working on iteration " << rep << " of " << opt::num_runs << std::endl;
    for (int k = 1; k <= 1; ++k) {
      for (auto& c : coverages) {
	for (auto& E : snv_error_rates) {
	  for (auto& D : del_error_rates) {
	    for (auto& I : ins_error_rates) {
	  	  
	  // make the read vector
	  ReadSim rs;
	  rs.addAllele(local_ref, 1);
	  
	  // sample reads randomly
	  //std::cerr << "...making read vector" << std::endl;
	  std::vector<std::string> reads;  
	  rs.sampleReadsToCoverage(reads, c, E, I, D, opt::readlen);

	  // sample paired reads
	  //std::cerr << "...making paired-end read vector" << std::endl;
	  std::vector<std::string> reads1;
	  std::vector<std::string> reads2;
	  rs.samplePairedEndReadsToCoverage(reads1, reads2, c, E, I, D, opt::readlen, 350, 50);
	  assert(reads1.size() == reads2.size());

	  // align these reads to the local_seq
	  //std::cerr << "...realigned to local_seq" << std::endl;
	  SnowTools::BamReadVector reads_to_local;
	  int count = 0;
	  for (auto& i : reads) {
	    if (i.find("N") == std::string::npos) {
	      SnowTools::BamReadVector read_hits;
	      local_bwa.alignSingleSequence(i, "read_" + std::to_string(++count), read_hits, false);
	      if (read_hits.size())
		reads_to_local.push_back(read_hits[0]);
	    }
	  }
	  
	  // kmer filter the reads
	  KmerFilter kmer;
	  if (k == 1)
	    kmer.correctReads(reads_to_local);
	  
	  //std::cerr << " Attempted align of " << reads.size() << " to local_seq. Got hits on " << reads_to_local.size() << std::endl;
	  
	  // make plot of reads to contig
	  //SnowTools::AlignedContig sa(self_align);  
	  //sa.alignReads(reads_to_local);
	  
	  // assemble them
	  //std::cerr << "...assembling" << std::endl;
	  double error_rate = 0;
	  if (k == 0)
	    error_rate = 0.05;
	  int min_overlap = 35;
	  SnowmanAssemblerEngine engine("test", error_rate, min_overlap, opt::readlen);
	  engine.fillReadTable(reads_to_local);
	  engine.performAssembly();
	  
	  // align them back
	  SnowTools::BamReadVector contigs_to_local;
	  for (auto& i : engine.getContigs()) {
	    SnowTools::BamReadVector ct_alignments;
	    local_bwa.alignSingleSequence(i.getSeq(), i.getID(), ct_alignments, false);
	    SnowTools::AlignedContig ac(ct_alignments);
	    ac.alignReads(reads_to_local);
	    //std::cout << ac;
	    contigs_to_local.insert(contigs_to_local.begin(), ct_alignments.begin(), ct_alignments.end());
	  }
	  
	  // write the results
	  SnowTools::GRC grc(contigs_to_local);
	  grc.mergeOverlappingIntervals();
	  double width = 0;
	  for (auto& i : grc)
	    width = std::max(width, (double)i.width());
	  width = width / local_ref.length();
	  std::cout << c << "\t" << reads_to_local.size() << "\t" << engine.getContigs().size() << 
	    "\t" << grc.size() << "\t" << width << "\t" << k << "\t" << E << std::endl;
	  
	  if (k == 1 && c == 20 && E == 0.01) {
	    // write out the contig to local ref bam
	    SnowTools::BamWalker bw2;
	    bw2.SetWriteHeader(local_bwa.HeaderFromIndex());
	    bw2.OpenWriteBam("contigs_to_ref.bam");
	    for (auto& i : contigs_to_local)
	      bw2.WriteAlignment(i);
	    
	    // write the paired end fasta
	    std::ofstream pe1;
	    pe1.open("paired_end1.fa", std::ios::out);
	    size_t ccc= 0;
	    for (auto& i : reads1)
	      pe1 << ">r" << ccc++ << std::endl << i << std::endl;
	    pe1.close();

	    std::ofstream pe2;
	    pe2.open("paired_end2.fa", std::ios::out);
	    ccc= 0;
	    for (auto& i : reads2)
	      pe2 << ">r" << ccc++ << std::endl << i << std::endl;
	    pe2.close();
	    
	    // write out the read to local ref aligned bam
	    SnowTools::BamWalker bw;
	    bw.SetWriteHeader(local_bwa.HeaderFromIndex());
	    bw.OpenWriteBam("reads_to_ref_" + std::to_string(c) + ".bam");
	    for (auto& i : reads_to_local)
	      bw.WriteAlignment(i);
	    
	    SnowTools::BamWalker bwk;
	    bwk.SetWriteHeader(local_bwa.HeaderFromIndex());
	    bwk.OpenWriteBam("k.bam");      
	    for (auto& i : reads_to_local) {
	      std::string kc = i.GetZTag("KC");
	      if (kc.length())
		i.SetSequence(kc);
	      bwk.WriteAlignment(i);
	    }
	    
	  }
	    } // end kmer loop
	  } // end error loop
	} // end coverage
      }
    }
  }
}

  
    void parseBenchmarkOptions(int argc, char** argv) {
    
    bool die = false;

  if (argc < 2) 
    die = true;

  std::string del_er;
  std::string snv_er;
  std::string ins_er;
  std::string covs;
  std::string frac;

  std::string t;

  for (char c; (c = getopt_long(argc, argv, shortopts, longopts, NULL)) != -1;) {
    std::istringstream arg(optarg != NULL ? optarg : "");
    switch (c) {
    case 'h': die = true; break;
    case OPT_ASSEMBLY: opt::mode = OPT_ASSEMBLY; break;
    case 'G': arg >> opt::refgenome; break;
    case 'n': arg >> opt::num_runs; break;
    case 's': arg >> opt::seed; break;
    case 'c': arg >> covs; break;
    case 'k': arg >> opt::regionFile; break;
    case 'b': arg >> opt::bam; break;
    case 'E': arg >> snv_er; break;
    case 'R': arg >> opt::nbreaks; break;
    case 'X': arg >> opt::nindels; break;
    case 'D': arg >> del_er; break;
    case 'I': arg >> ins_er; break;
    case 'A': arg >> opt::string_id; break;
    case 'f': arg >> frac; break;
    case OPT_SIMBREAKS: opt::mode = OPT_SIMBREAKS; break;
    case OPT_SPLITBAM: opt::mode = OPT_SPLITBAM; break;
    case OPT_ISIZE_MEAN: arg >> opt::isize_mean; break;
    case OPT_ISIZE_SD: arg >> opt::isize_sd; break;
    default: die= true; 
    }
  }

  if (die) {
    std::cerr << "\n" << BENCHMARK_USAGE_MESSAGE;
    exit(EXIT_FAILURE);
  }

  // parse the error rates
  snv_error_rates = parseErrorRates(snv_er);
  del_error_rates = parseErrorRates(del_er);
  ins_error_rates = parseErrorRates(ins_er);
  coverages = parseErrorRates(covs);

  // parse the fractions string or read file
  if (!SnowTools::read_access_test(frac))
    fractions = parseErrorRates(frac);
  else
    opt::frac_bed_file = frac;

  // set the default error rates
  if (!snv_error_rates.size())
    snv_error_rates.push_back(DEFAULT_SNV_RATE);
  if (!del_error_rates.size())
    del_error_rates.push_back(DEFAULT_DEL_RATE);
  if (!ins_error_rates.size())
    ins_error_rates.push_back(DEFAULT_INS_RATE);
  if (!coverages.size())
    coverages.push_back(DEFAULT_COV);
  if ((!fractions.size() && !opt::frac_bed_file.length()) && opt::mode == OPT_SPLITBAM) {
    std::cerr << "Error: Must specify fractions to split into with -f (e.g. -f 0.1,0.8), or as BED file" << std::endl;
    exit(EXIT_FAILURE);
  }
  
    }

// parse the error rates string from the options
  std::vector<double> parseErrorRates(const std::string& s) {
    
    std::vector<double> out;
  
  std::istringstream is(s);
  std::string val;
  while(std::getline(is, val, ',')) {
    try {
      out.push_back(std::stod(val));
    } catch (...) {
      std::cerr << "Could not convert " << val << " to number" << std::endl;
      exit(EXIT_FAILURE);
    }
  }
    
  return out;
}
 
 std::string errorRateString(const std::vector<double>& v, const std::string& name) {
  
  std::stringstream ss;
  ss << "        " << name << ":";
  for (auto& i : v)
    ss << " " << i << ",";
  
  return SnowTools::cutLastChar(ss.str());
  
}