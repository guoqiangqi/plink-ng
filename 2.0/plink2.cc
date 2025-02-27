// This file is part of PLINK 2.00, copyright (C) 2005-2019 Shaun Purcell,
// Christopher Chang.
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.


#include "plink2_compress_stream.h"
#include "plink2_export.h"
#include "plink2_fasta.h"
#include "plink2_filter.h"
#include "plink2_glm.h"
#include "plink2_help.h"
#include "plink2_import.h"
#include "plink2_ld.h"
#include "plink2_matrix_calc.h"
#include "plink2_misc.h"
#include "plink2_psam.h"
#include "plink2_pvar.h"
#include "plink2_random.h"
#include "plink2_set.h"
#include "plink2_zstfile.h"

#include <time.h>  // time()
#include <unistd.h>  // unlink()

#ifdef __APPLE__
#  include <fenv.h>  // fesetenv()
#endif

#ifdef __cplusplus
namespace plink2 {
#endif

static const char ver_str[] = "PLINK v2.00a2"
#ifdef NOLAPACK
  "NL"
#endif
#ifdef __LP64__
#  ifdef LAPACK_ILP64
    "LM"
#  endif
#  ifdef USE_AVX2
    " AVX2"
#  else
#    ifdef USE_SSE42
      " SSE4.2"
#    else
      " 64-bit"
#    endif
#  endif
#else
  " 32-bit"
#endif

#ifdef USE_MKL
  " Intel"
#endif
  " (15 Nov 2019)";
static const char ver_str2[] =
  // include leading space if day < 10, so character length stays the same
  ""
#ifndef LAPACK_ILP64
  "  "
#endif
#ifndef USE_MKL
  "      "
#endif
#ifdef USE_AVX2
  "  "
#endif
#ifndef NOLAPACK
  "  "
#endif
  "   www.cog-genomics.org/plink/2.0/\n"
  "(C) 2005-2019 Shaun Purcell, Christopher Chang   GNU General Public License v3\n";
static const char errstr_append[] = "For more info, try \"" PROG_NAME_STR " --help <flag name>\" or \"" PROG_NAME_STR " --help | more\".\n";

#ifndef NOLAPACK
static const char notestr_null_calc2[] = "Commands include --rm-dup list, --make-bpgen, --export, --freq, --geno-counts,\n--sample-counts, --missing, --hardy, --indep-pairwise, --ld, --sample-diff,\n--make-king, --king-cutoff, --write-samples, --write-snplist, --make-grm-list,\n--pca, --glm, --adjust-file, --score, --variant-score, --genotyping-rate,\n--pgen-info, --validate, and --zst-decompress.\n\n\"" PROG_NAME_STR " --help | more\" describes all functions.\n";
#else
static const char notestr_null_calc2[] = "Commands include --rm-dup list, --make-bpgen, --export, --freq, --geno-counts,\n--sample-counts, --missing, --hardy, --indep-pairwise, --ld, --sample-diff,\n--make-king, --king-cutoff, --write-samples, --write-snplist, --make-grm-list,\n--glm, --adjust-file, --score, --variant-score, --genotyping-rate, --pgen-info,\n--validate, and --zst-decompress.\n\n\"" PROG_NAME_STR " --help | more\" describes all functions.\n";
#endif

// covar-variance-standardize + terminating null
CONSTI32(kMaxFlagBlen, 27);

FLAGSET_DEF_START()
  kfLoadParams0,
  kfLoadParamsPgen = (1 << 0),
  kfLoadParamsPsam = (1 << 1),
  kfLoadParamsPvar = (1 << 2),
  kfLoadParamsPfileAll = (kfLoadParamsPgen | kfLoadParamsPsam | kfLoadParamsPvar)
FLAGSET_DEF_END(LoadParams);

FLAGSET_DEF_START()
  kfXload0,
  kfXloadVcf = (1 << 0),
  kfXloadBcf = (1 << 1),
  kfXloadOxSample = (1 << 2),
  kfXloadOxGen = (1 << 3),
  kfXloadOxBgen = (1 << 4),
  kfXloadOxHaps = (1 << 5),
  kfXloadOxLegend = (1 << 6),
  kfXloadPlink1Dosage = (1 << 7),
  kfXloadMap = (1 << 8),
  kfXloadGenDummy = (1 << 9)
FLAGSET_DEF_END(Xload);


// assume for now that .pgen must always be accompanied by both .pvar and .psam
// currently does double-duty in tracking dependencies
FLAGSET64_DEF_START()
  kfFilter0,
  kfFilterAllReq = (1 << 0),
  kfFilterPvarReq = (1 << 1),
  kfFilterPsamReq = (1 << 2),
  kfFilterOpportunisticPgen = (1 << 3),
  kfFilterNonrefFlagsNeeded = (1 << 4),
  kfFilterNonrefFlagsNeededSet = kfFilterNonrefFlagsNeeded | kfFilterOpportunisticPgen,
  kfFilterNoSplitChr = (1 << 5),
  kfFilterExclFemales = (1 << 6),
  kfFilterExclMales = (1 << 7),
  kfFilterExclNosex = (1 << 8),
  kfFilterExclFounders = (1 << 9),
  kfFilterExclNonfounders = (1 << 10),
  kfFilterSnpsOnly = (1 << 11),
  kfFilterSnpsOnlyJustAcgt = (1 << 12),
  kfFilterExtractBed0 = (1 << 13),
  kfFilterExtractBed1 = (1 << 14),
  kfFilterExtractIntersectBed0 = (1 << 15),
  kfFilterExtractIntersectBed1 = (1 << 16),
  kfFilterExcludeBed0 = (1 << 17),
  kfFilterExcludeBed1 = (1 << 18)
FLAGSET64_DEF_END(FilterFlags);

FLAGSET64_DEF_START()
  kfCommand10,
  kfCommand1MakePlink2 = (1 << 0),
  kfCommand1Exportf = (1 << 1),
  kfCommand1MakeKing = (1 << 2),
  kfCommand1KingCutoff = (1 << 3),
  kfCommand1MissingReport = (1 << 4),
  kfCommand1WriteSnplist = (1 << 5),
  kfCommand1AlleleFreq = (1 << 6),
  kfCommand1GenoCounts = (1 << 7),
  kfCommand1Hardy = (1 << 8),
  kfCommand1LdPrune = (1 << 9),
  kfCommand1Pca = (1 << 10),
  kfCommand1Glm = (1 << 11),
  kfCommand1MakeRel = (1 << 12),
  kfCommand1Validate = (1 << 13),
  kfCommand1GenotypingRate = (1 << 14),
  kfCommand1Score = (1 << 15),
  kfCommand1WriteCovar = (1 << 16),
  kfCommand1WriteSamples = (1 << 17),
  kfCommand1Ld = (1 << 18),
  kfCommand1PgenInfo = (1 << 19),
  kfCommand1RmDupList = (1 << 20),
  kfCommand1Sdiff = (1 << 21),
  kfCommand1SampleCounts = (1 << 22),
  kfCommand1Vscore = (1 << 23)
FLAGSET64_DEF_END(Command1Flags);

// this is a hybrid, only kfSortFileSid is actually a flag
FLAGSET_DEF_START()
  kfSort0,
  kfSortNone = (1 << 0),
  kfSortNatural = (1 << 1),
  kfSortAscii = (1 << 2),
  kfSortFile = (1 << 3),
  kfSortFileSid = (1 << 4)
FLAGSET_DEF_END(SortFlags);

void PgenInfoPrint(const char* pgenname, const PgenFileInfo* pgfip, PgenHeaderCtrl header_ctrl, uint32_t max_allele_ct) {
  logerrprintfww("--pgen-info on %s:\n", pgenname);
  logerrprintf("  Variants: %u\n", pgfip->raw_variant_ct);
  logerrprintf("  Samples: %u\n", pgfip->raw_sample_ct);
  const uint32_t nonref_flags_status = header_ctrl >> 6;
  if (!nonref_flags_status) {
    logerrputs("  REF allele known/provisional status not stored in .pgen\n");
  } else if (nonref_flags_status == 1) {
    logerrputs("  REF alleles are all known\n");
  } else if (nonref_flags_status == 2) {
    logerrputs("  REF alleles are all provisional\n");
  } else {
    // could report exact counts of each
    logerrputs("  REF alleles are a mix of known and provisional\n");
  }
  if (max_allele_ct >= UINT32_MAXM1) {
    if (max_allele_ct == UINT32_MAX) {
      logerrputs("  Maximum allele count for a single variant: >2, not explicitly stored\n");
    } else {
      logerrputs("  Maximum allele count for a single variant: not explicitly stored\n");
    }
  } else {
    logerrprintf("  Maximum allele count for a single variant: %u\n", max_allele_ct);
  }
  if (pgfip->gflags & kfPgenGlobalHardcallPhasePresent) {
    logerrputs("  Explicitly phased hardcalls present\n");
  } else {
    logerrputs("  No hardcalls are explicitly phased\n");
  }
  if (pgfip->gflags & kfPgenGlobalDosagePresent) {
    if (pgfip->gflags & kfPgenGlobalDosagePhasePresent) {
      logerrputs("  Explicitly phased dosages present\n");
    } else {
      logerrputs("  Dosage present, none explicitly phased\n");
    }
  } else {
    logerrputs("  No dosages present\n");
  }
}

PglErr PgenInfoStandalone(const char* pgenname) {
  PgenFileInfo pgfi;
  PglErr reterr = kPglRetSuccess;
  PreinitPgfi(&pgfi);
  {
    PgenHeaderCtrl header_ctrl;
    uintptr_t cur_alloc_cacheline_ct;
    reterr = PgfiInitPhase1(pgenname, UINT32_MAX, UINT32_MAX, 0, &header_ctrl, &pgfi, &cur_alloc_cacheline_ct, g_logbuf);
    if (unlikely(reterr)) {
      if ((reterr == kPglRetSampleMajorBed) || (reterr == kPglRetImproperFunctionCall)) {
        logerrputs("Warning: Skipping --pgen-info since a .bed file was provided.\n");
        reterr = kPglRetSuccess;
      } else {
        logerrputsb();
      }
      goto PgenInfoStandalone_ret_1;
    }
    const uint32_t raw_variant_ct = pgfi.raw_variant_ct;
    const uint32_t raw_variant_ctl = BitCtToWordCt(raw_variant_ct);
    unsigned char* pgfi_alloc;
    if (unlikely(
            bigstack_alloc_uc(cur_alloc_cacheline_ct * kCacheline, &pgfi_alloc) ||
            bigstack_alloc_w(raw_variant_ct + 1, &pgfi.allele_idx_offsets) ||
            bigstack_alloc_w(raw_variant_ctl, &pgfi.nonref_flags))) {
      reterr = kPglRetNomem;
      goto PgenInfoStandalone_ret_1;
    }
    uintptr_t pgr_alloc_cacheline_ct = 0;
    uint32_t max_vrec_width;
    reterr = PgfiInitPhase2(header_ctrl, 0, 0, 1, 0, raw_variant_ct, &max_vrec_width, &pgfi, pgfi_alloc, &pgr_alloc_cacheline_ct, g_logbuf);
    if (unlikely(reterr)) {
      logerrputsb();
      goto PgenInfoStandalone_ret_1;
    }
    uint32_t max_allele_ct = 2;
    if (pgfi.gflags & kfPgenGlobalMultiallelicHardcallFound) {
      max_allele_ct = UINT32_MAX;
    } else if (pgfi.gflags & kfPgenGlobalDosagePresent) {
      max_allele_ct = UINT32_MAXM1;
    }
    PgenInfoPrint(pgenname, &pgfi, header_ctrl, max_allele_ct);
  }
 PgenInfoStandalone_ret_1:
  CleanupPgfi2(pgenname, &pgfi, &reterr);
  // no BigstackReset() needed?
  return reterr;
}

typedef struct Plink2CmdlineStruct {
  NONCOPYABLE(Plink2CmdlineStruct);
  MiscFlags misc_flags;

  // filter_flags tracks some info about flags which may cause the .pgen part
  // of --make-pgen's output to change (affects sample_include/variant_include,
  // etc.)  other .psam/.pvar/.pgen dependency info is now tracked by
  // dependency_flags (which is set to a superset of filter_flags at the end of
  // command-line parsing).
  // (filter_flags & kfFilterPsamReq) and (filter_flags & kfFilterPvarReq) can
  // now be used to detect whether sample_include or variant_include may be
  // modified.
  FilterFlags filter_flags;
  FilterFlags dependency_flags;

  Command1Flags command_flags1;
  PvarPsamFlags pvar_psam_flags;
  SortFlags sample_sort_flags;
  SortFlags sort_vars_flags;
  GrmFlags grm_flags;
  PcaFlags pca_flags;
  WriteCovarFlags write_covar_flags;
  PhenoTransformFlags pheno_transform_flags;
  FaFlags fa_flags;
  RangeList snps_range_list;
  RangeList exclude_snps_range_list;
  RangeList pheno_range_list;
  RangeList covar_range_list;
  RangeList vscore_col_idx_range_list;
  FamCol fam_cols;
  UpdateSexInfo update_sex_info;
  LdInfo ld_info;
  SdiffInfo sdiff_info;
  KingFlags king_flags;
  double king_cutoff;
  double king_table_filter;
  double king_table_subset_thresh;
  FreqRptFlags freq_rpt_flags;
  MissingRptFlags missing_rpt_flags;
  GenoCountsFlags geno_counts_flags;
  HardyFlags hardy_flags;
  SampleCountsFlags sample_counts_flags;
  RecoverVarIdsFlags recover_var_ids_flags;
  VscoreFlags vscore_flags;
  RmDupMode rmdup_mode;
  STD_ARRAY_DECL(FreqFilterMode, 4, filter_modes);
  GlmInfo glm_info;
  AdjustInfo adjust_info;
  ScoreInfo score_info;
  APerm aperm;
  CmpExpr keep_if_expr;
  CmpExpr remove_if_expr;
  CmpExpr extract_if_info_expr;
  CmpExpr exclude_if_info_expr;
  ExtractFcolInfo extract_fcol_info;
  ExportfInfo exportf_info;
  double ci_size;
  float var_min_qual;
  uint32_t splitpar_bound1;
  uint32_t splitpar_bound2;
  uint32_t new_variant_id_max_allele_slen;
  uint32_t update_sex_colm2;

  // maybe support BGEN v1.2-style variable-precision dosages later, at which
  // point these should be floating-point numbers; but let's first see what we
  // gain from v1.1 fixed-point arithmetic
  uint32_t hard_call_thresh;
  uint32_t dosage_erase_thresh;

  double ln_pfilter;
  double output_min_ln;
  double vif_thresh;
  double mind_thresh;
  double geno_thresh;
  double hwe_thresh;
  double mach_r2_min;
  double mach_r2_max;
  double minimac3_r2_min;
  double minimac3_r2_max;
  double min_maf;
  double max_maf;
  double thin_keep_prob;
  double thin_keep_sample_prob;
  uint64_t min_allele_dosage;
  uint64_t max_allele_dosage;
  int32_t missing_pheno;
  int32_t from_bp;
  int32_t to_bp;
  int32_t window_bp;
  uint32_t pca_ct;
  uint32_t xchr_model;
  uint32_t max_thread_ct;
  uint32_t parallel_idx;
  uint32_t parallel_tot;
  uint32_t mwithin_val;
  uint32_t min_bp_space;
  uint32_t thin_keep_ct;
  uint32_t thin_keep_sample_ct;
  uint32_t keep_fcol_num;
  uint32_t filter_min_allele_ct;
  uint32_t filter_max_allele_ct;

  char* var_filter_exceptions_flattened;
  char* varid_template_str;
  char* varid_multi_template_str;
  char* varid_multi_nonsnp_template_str;
  char* missing_varid_match;
  char* varid_from;
  char* varid_to;
  char* varid_snp;
  char* varid_exclude_snp;
  char* pheno_fname;
  char* covar_fname;
  char* extract_fnames;
  char* extract_intersect_fnames;
  char* exclude_fnames;
  char* keep_fnames;
  char* keepfam_fnames;
  char* remove_fnames;
  char* removefam_fnames;
  char* sample_sort_fname;
  char* freq_ref_binstr;
  char* freq_alt1_binstr;
  char* glm_local_covar_fname;
  char* glm_local_pvar_fname;
  char* glm_local_psam_fname;
  char* read_freq_fname;
  char* within_fname;
  char* catpheno_name;
  char* family_missing_catname;
  char* keep_cats_fname;
  char* keep_cat_names_flattened;
  char* keep_cat_phenoname;
  char* remove_cats_fname;
  char* remove_cat_names_flattened;
  char* remove_cat_phenoname;
  char* split_cat_phenonames_flattened;
  char* require_pheno_flattened;
  char* require_covar_flattened;
  char* vstd_flattened;
  char* quantnorm_flattened;
  char* covar_quantnorm_flattened;
  char* loop_cats_phenoname;
  char* fa_fname;
  char* king_table_subset_fname;
  char* require_info_flattened;
  char* require_no_info_flattened;
  char* keep_fcol_fname;
  char* keep_fcol_flattened;
  char* keep_fcol_name;
  char* update_alleles_fname;
  char* update_sample_ids_fname;
  char* update_parental_ids_fname;
  char* recover_var_ids_fname;
  char* vscore_fname;
  TwoColParams* ref_allele_flag;
  TwoColParams* alt1_allele_flag;
  TwoColParams* update_map_flag;
  TwoColParams* update_name_flag;
} Plink2Cmdline;

// er, probably time to just always initialize this...
uint32_t SingleVariantLoaderIsNeeded(const char* king_cutoff_fprefix, Command1Flags command_flags1, MakePlink2Flags make_plink2_flags, RmDupMode rmdup_mode, double hwe_thresh) {
  return (command_flags1 & (kfCommand1Exportf | kfCommand1MakeKing | kfCommand1GenoCounts | kfCommand1LdPrune | kfCommand1Validate | kfCommand1Pca | kfCommand1MakeRel | kfCommand1Glm | kfCommand1Score | kfCommand1Ld | kfCommand1Hardy | kfCommand1Sdiff)) || ((command_flags1 & kfCommand1MakePlink2) && (make_plink2_flags & kfMakePgen)) || ((command_flags1 & kfCommand1KingCutoff) && (!king_cutoff_fprefix)) || (rmdup_mode != kRmDup0) || (hwe_thresh != 0.0);
}


uint32_t DecentAlleleFreqsAreNeeded(Command1Flags command_flags1, ScoreFlags score_flags) {
  return (command_flags1 & (kfCommand1Pca | kfCommand1MakeRel)) || ((command_flags1 & kfCommand1Score) && ((!(score_flags & kfScoreNoMeanimpute)) || (score_flags & (kfScoreCenter | kfScoreVarianceStandardize))));
}

// not actually needed for e.g. --hardy, --hwe, etc. if no multiallelic
// variants are retained, but let's keep this simpler for now
uint32_t MajAllelesAreNeeded(Command1Flags command_flags1, GlmFlags glm_flags) {
  return (command_flags1 & (kfCommand1LdPrune | kfCommand1Pca | kfCommand1MakeRel | kfCommand1Ld)) || ((command_flags1 & kfCommand1Glm) && (!(glm_flags & kfGlmOmitRef)));
}

// only needs to cover cases not captured by DecentAlleleFreqsAreNeeded() or
// MajAllelesAreNeeded()
uint32_t IndecentAlleleFreqsAreNeeded(Command1Flags command_flags1, double min_maf, double max_maf) {
  // Vscore could go either here or in the decent bucket
  return (command_flags1 & (kfCommand1AlleleFreq | kfCommand1Vscore)) || (min_maf != 0.0) || (max_maf != 1.0);
}


uint32_t GetFirstHaploidUidx(const ChrInfo* cip, UnsortedVar vpos_sortstatus) {
  // returns 0x7fffffff if no X/haploid chromosomes present
  if (!(vpos_sortstatus & kfUnsortedVarSplitChr)) {
    const uint32_t chr_ct = cip->chr_ct;
    for (uint32_t chr_fo_idx = 0; chr_fo_idx != chr_ct; ++chr_fo_idx) {
      const uint32_t chr_idx = cip->chr_file_order[chr_fo_idx];
      if (IsSet(cip->haploid_mask, chr_idx)) {
        return cip->chr_fo_vidx_start[chr_fo_idx];
      }
    }
  }
  return 0x7fffffff;
}

uint32_t AlleleDosagesAreNeeded(MiscFlags misc_flags, uint32_t afreq_needed, uint64_t min_allele_dosage, uint64_t max_allele_dosage, uint32_t* regular_freqcounts_neededp) {
  if (!(misc_flags & kfMiscNonfounders)) {
    return 0;
  }
  if ((misc_flags & kfMiscMajRef) || min_allele_dosage || (max_allele_dosage != UINT32_MAX)) {
    *regular_freqcounts_neededp = 1;
    return 1;
  }
  return afreq_needed;
}

uint32_t FounderAlleleDosagesAreNeeded(MiscFlags misc_flags, uint32_t afreq_needed, uint64_t min_allele_dosage, uint64_t max_allele_dosage, uint32_t* regular_freqcounts_neededp) {
  if (misc_flags & kfMiscNonfounders) {
    return 0;
  }
  if ((misc_flags & kfMiscMajRef) || min_allele_dosage || (max_allele_dosage != (~0LLU))) {
    *regular_freqcounts_neededp = 1;
    return 1;
  }
  return afreq_needed;
}

uint32_t SampleMissingDosageCtsAreNeeded(MiscFlags misc_flags, uint32_t smaj_missing_geno_report_requested, double mind_thresh, MissingRptFlags missing_rpt_flags) {
  return ((mind_thresh != 1.0) && (misc_flags & kfMiscMindDosage)) || (smaj_missing_geno_report_requested && (missing_rpt_flags & (kfMissingRptScolNmissDosage | kfMissingRptScolFmissDosage)));
}

uint32_t VariantMissingHcCtsAreNeeded(Command1Flags command_flags1, MiscFlags misc_flags, double geno_thresh, MissingRptFlags missing_rpt_flags) {
  return ((command_flags1 & kfCommand1GenotypingRate) && (!(misc_flags & kfMiscGenotypingRateDosage))) || ((command_flags1 & kfCommand1MissingReport) && (missing_rpt_flags & (kfMissingRptVcolNmiss | kfMissingRptVcolNmissHh | kfMissingRptVcolHethap | kfMissingRptVcolFmiss | kfMissingRptVcolFmissHh | kfMissingRptVcolFhethap))) || ((geno_thresh != 1.0) && (!(misc_flags & kfMiscGenoDosage)));
}

uint32_t VariantHethapCtsAreNeeded(Command1Flags command_flags1, MiscFlags misc_flags, double geno_thresh, MissingRptFlags missing_rpt_flags, uint32_t first_hap_uidx) {
  return (first_hap_uidx != 0x7fffffff) && (((command_flags1 & kfCommand1MissingReport) && (missing_rpt_flags & (kfMissingRptVcolNmissHh | kfMissingRptVcolHethap | kfMissingRptVcolFmissHh | kfMissingRptVcolFhethap))) || ((geno_thresh != 1.0) && (!(misc_flags & kfMiscGenoHhMissing))));
}

uint32_t VariantMissingDosageCtsAreNeeded(Command1Flags command_flags1, MiscFlags misc_flags, double geno_thresh, MissingRptFlags missing_rpt_flags) {
  return ((command_flags1 & kfCommand1GenotypingRate) && (misc_flags & kfMiscGenotypingRateDosage)) || ((command_flags1 & kfCommand1MissingReport) && (!(missing_rpt_flags & kfMissingRptSampleOnly)) && (missing_rpt_flags & (kfMissingRptVcolNmissDosage | kfMissingRptVcolFmissDosage))) || ((geno_thresh != 1.0) && (misc_flags & kfMiscGenoDosage));
}

// can simplify --geno-counts all-biallelic case, but let's first make sure the
// general case works for multiallelic variants
uint32_t RawGenoCtsAreNeeded(Command1Flags command_flags1, MiscFlags misc_flags, double hwe_thresh) {
  return (command_flags1 & kfCommand1GenoCounts) || ((misc_flags & kfMiscNonfounders) && ((command_flags1 & kfCommand1Hardy) || (hwe_thresh != 0.0)));
}

uint32_t FounderRawGenoCtsAreNeeded(Command1Flags command_flags1, MiscFlags misc_flags, double hwe_thresh) {
  return (!(misc_flags & kfMiscNonfounders)) && ((command_flags1 & kfCommand1Hardy) || (hwe_thresh != 0.0));
}

uint32_t InfoReloadIsNeeded(Command1Flags command_flags1, PvarPsamFlags pvar_psam_flags, ExportfFlags exportf_flags, RmDupMode rmdup_mode) {
  // add kfExportfBcf later
  return ((command_flags1 & kfCommand1MakePlink2) && (pvar_psam_flags & kfPvarColXinfo)) || ((command_flags1 & kfCommand1Exportf) && (exportf_flags & kfExportfVcf)) || (rmdup_mode != kRmDup0);
}

uint32_t GrmKeepIsNeeded(Command1Flags command_flags1, PcaFlags pca_flags) {
  return ((command_flags1 & kfCommand1Pca) && (!(pca_flags & kfPcaApprox)));
}

void ReportGenotypingRate(const uintptr_t* variant_include, const ChrInfo* cip, const uint32_t* variant_missing_cts, uint32_t raw_sample_ct, uint32_t sample_ct, uint32_t male_ct, uint32_t variant_ct, uint32_t is_dosage) {
  // defined the same way as PLINK 1.x, to allow this to serve as a sanity
  // check
  // trivial to multithread this if it ever matters
  uint64_t tot_nony_missing = 0;
  uint64_t tot_y_missing = 0;
  uint64_t cur_tot_missing = 0;
  uint32_t y_start = UINT32_MAX;
  uint32_t y_end = UINT32_MAX;
  uint32_t variant_ct_y = 0;
  uint32_t y_code;
  if (XymtExists(cip, kChrOffsetY, &y_code)) {
    const uint32_t y_chr_fo_idx = cip->chr_idx_to_foidx[y_code];
    y_start = cip->chr_fo_vidx_start[y_chr_fo_idx];
    y_end = cip->chr_fo_vidx_start[y_chr_fo_idx + 1];
    variant_ct_y = PopcountBitRange(variant_include, y_start, y_end);
  }
  uint32_t y_thresh = y_start;
  uintptr_t variant_uidx_base = 0;
  uintptr_t cur_bits = variant_include[0];
  uint32_t is_y = 0;
  for (uint32_t variant_idx = 0; variant_idx != variant_ct; ++variant_idx) {
    const uint32_t variant_uidx = BitIter1(variant_include, &variant_uidx_base, &cur_bits);
    if (variant_uidx >= y_thresh) {
      if (is_y) {
        tot_y_missing = cur_tot_missing;
      } else {
        tot_nony_missing = cur_tot_missing;
      }
      is_y = (variant_uidx < y_end);
      cur_tot_missing = 0;
      if (is_y) {
        y_thresh = y_end;
      } else {
        y_thresh = UINT32_MAX;
      }
    }
    cur_tot_missing += variant_missing_cts[variant_uidx];
  }
  if (is_y) {
    tot_y_missing = cur_tot_missing;
  } else {
    tot_nony_missing += cur_tot_missing;
  }
  if ((!tot_y_missing) && (!tot_nony_missing)) {
    logprintf("Total (%s) genotyping rate %sis exactly 1.\n", is_dosage? "dosage" : "hardcall", (raw_sample_ct != sample_ct)? "in remaining samples " : "");
    return;
  }
  double genotyping_rate;
  if (male_ct && variant_ct_y) {
    const uint64_t nony_possible_obs = (variant_ct - variant_ct_y) * S_CAST(uint64_t, sample_ct);
    const uint64_t y_possible_obs = variant_ct_y * S_CAST(uint64_t, male_ct);
    genotyping_rate = u63tod(nony_possible_obs - tot_nony_missing) / u31tod(sample_ct) + u63tod(y_possible_obs - tot_y_missing) / u31tod(male_ct);
    genotyping_rate /= u31tod(variant_ct);
  } else {
    variant_ct -= variant_ct_y;
    const uint64_t denom = variant_ct * S_CAST(uint64_t, sample_ct);
    genotyping_rate = u63tod(denom - tot_nony_missing) / u63tod(denom);
  }
  if (genotyping_rate >= 0.9999995) {
    logprintf("Total (%s) genotyping rate %sis in [0.9999995, 1).\n", is_dosage? "dosage" : "hardcall", (raw_sample_ct != sample_ct)? "in remaining samples " : "");
  } else {
    logprintf("Total (%s) genotyping rate %sis %g.\n", is_dosage? "dosage" : "hardcall", (raw_sample_ct != sample_ct)? "in remaining samples " : "", genotyping_rate);
  }
}

PglErr ApplyVariantBpFilters(const char* extract_fnames, const char* extract_intersect_fnames, const char* exclude_fnames, const ChrInfo* cip, const uint32_t* variant_bps, int32_t from_bp, int32_t to_bp, uint32_t raw_variant_ct, FilterFlags filter_flags, UnsortedVar vpos_sortstatus, uint32_t max_thread_ct, uintptr_t* variant_include, uint32_t* variant_ct_ptr) {
  if (!(*variant_ct_ptr)) {
    return kPglRetSuccess;
  }
  if ((from_bp != -1) || (to_bp != -1)) {
    if (unlikely(vpos_sortstatus & kfUnsortedVarBp)) {
      logerrputs("Error: --from-bp and --to-bp require a sorted .pvar/.bim.  Retry this command\nafter using --make-pgen/--make-bed + --sort-vars to sort your data.\n");
      return kPglRetInconsistentInput;
    }
    const uint32_t chr_idx = AdvBoundedTo1Bit(cip->chr_mask, 0, kChrRawEnd);

    // this function shouldn't be called unless variant_ct is nonzero
    assert(chr_idx != kChrRawEnd);

    const uint32_t chr_fo_idx = cip->chr_idx_to_foidx[chr_idx];
    uint32_t variant_uidx_start = cip->chr_fo_vidx_start[chr_fo_idx];
    uint32_t variant_uidx_end = cip->chr_fo_vidx_start[chr_fo_idx + 1];
    if (from_bp != -1) {
      const uint32_t from_offset = CountSortedSmallerU32(&(variant_bps[variant_uidx_start]), variant_uidx_end - variant_uidx_start, from_bp);
      variant_uidx_start += from_offset;
    }
    if ((to_bp != -1) && (variant_uidx_start < variant_uidx_end)) {
      const uint32_t to_offset = CountSortedSmallerU32(&(variant_bps[variant_uidx_start]), variant_uidx_end - variant_uidx_start, 1 + to_bp);
      variant_uidx_end = variant_uidx_start + to_offset;
    }
    if (variant_uidx_start) {
      ClearBitsNz(0, variant_uidx_start, variant_include);
    }
    if (variant_uidx_end < raw_variant_ct) {
      ClearBitsNz(variant_uidx_end, raw_variant_ct, variant_include);
    }
    *variant_ct_ptr = PopcountBitRange(variant_include, variant_uidx_start, variant_uidx_end);
  }
  if (extract_fnames && (filter_flags & (kfFilterExtractBed0 | kfFilterExtractBed1))) {
    if (unlikely(vpos_sortstatus & kfUnsortedVarBp)) {
      logerrputs("Error: '--extract bed0'/'--extract bed1' requires a sorted .pvar/.bim.  Retry\nthis command after using --make-pgen/--make-bed + --sort-vars to sort your\ndata.\n");
      return kPglRetInconsistentInput;
    }
    PglErr reterr = ExtractExcludeRange(extract_fnames, cip, variant_bps, raw_variant_ct, kVfilterExtract, (filter_flags / kfFilterExtractBed0) & 1, max_thread_ct, variant_include, variant_ct_ptr);
    if (unlikely(reterr)) {
      return reterr;
    }
  }
  if (extract_intersect_fnames && (filter_flags & (kfFilterExtractIntersectBed0 | kfFilterExtractIntersectBed1))) {
    if (unlikely(vpos_sortstatus & kfUnsortedVarBp)) {
      logerrputs("Error: '--extract-intersect bed0'/'--extract-intersect bed1' requires a sorted\n.pvar/.bim.  Retry this command after using --make-pgen/--make-bed +\n--sort-vars to sort your data.\n");
      return kPglRetInconsistentInput;
    }
    PglErr reterr = ExtractExcludeRange(extract_intersect_fnames, cip, variant_bps, raw_variant_ct, kVfilterExtractIntersect, (filter_flags / kfFilterExtractIntersectBed0) & 1, max_thread_ct, variant_include, variant_ct_ptr);
    if (unlikely(reterr)) {
      return reterr;
    }
  }
  if (exclude_fnames && (filter_flags & (kfFilterExcludeBed0 | kfFilterExcludeBed1))) {
    if (unlikely(vpos_sortstatus & kfUnsortedVarBp)) {
      logerrputs("Error: '--exclude bed0'/'--exclude bed1' requires a sorted .pvar/.bim.  Retry\nthis commandafter using --make-pgen/--make-bed + --sort-vars to sort your\ndata.\n");
      return kPglRetInconsistentInput;
    }
    PglErr reterr = ExtractExcludeRange(exclude_fnames, cip, variant_bps, raw_variant_ct, kVfilterExclude, (filter_flags / kfFilterExcludeBed0) & 1, max_thread_ct, variant_include, variant_ct_ptr);
    if (unlikely(reterr)) {
      return reterr;
    }
  }
  return kPglRetSuccess;
}

void UpdateSampleSubsets(const uintptr_t* sample_include, uint32_t raw_sample_ct, uint32_t sample_ct, uintptr_t* founder_info, uint32_t* founder_ct_ptr, uintptr_t* sex_nm, uintptr_t* sex_male, uint32_t* male_ct_ptr, uint32_t* nosex_ct_ptr) {
  const uint32_t raw_sample_ctl = BitCtToWordCt(raw_sample_ct);
  BitvecAnd(sample_include, raw_sample_ctl, founder_info);
  *founder_ct_ptr = PopcountWords(founder_info, raw_sample_ctl);
  BitvecAnd(sample_include, raw_sample_ctl, sex_male);
  *male_ct_ptr = PopcountWords(sex_male, raw_sample_ctl);
  BitvecAnd(sample_include, raw_sample_ctl, sex_nm);
  *nosex_ct_ptr = sample_ct - PopcountWords(sex_nm, raw_sample_ctl);
}

// command_flags2 will probably be needed before we're done
static_assert(kPglMaxAltAlleleCt == 254, "Plink2Core() --maj-ref needs to be updated.");
PglErr Plink2Core(const Plink2Cmdline* pcp, MakePlink2Flags make_plink2_flags, char* pgenname, char* psamname, char* pvarname, char* outname, char* outname_end, char* king_cutoff_fprefix, ChrInfo* cip, sfmt_t* sfmtp) {
  PhenoCol* pheno_cols = nullptr;
  PhenoCol* covar_cols = nullptr;
  PhenoCol* loop_cats_pheno_col = nullptr;
  char* pheno_names = nullptr;
  char* covar_names = nullptr;
  uint32_t pheno_ct = 0;
  uint32_t covar_ct = 0;
  PglErr reterr = kPglRetSuccess;
  PgenFileInfo pgfi;
  PgenReader simple_pgr;
  PreinitPgfi(&pgfi);
  PreinitPgr(&simple_pgr);
  {
    // this predicate will need to exclude --merge-list special case later
    uint32_t pvar_renamed = 0;
    if ((make_plink2_flags & (kfMakeBed | kfMakePgen)) || (pcp->exportf_info.flags & kfExportfIndMajorBed)) {
      uint32_t fname_slen;
#ifdef _WIN32
      fname_slen = GetFullPathName(pgenname, kPglFnamesize, g_textbuf, nullptr);
      if (unlikely((!fname_slen) || (fname_slen > kPglFnamesize)))
#else
      if (unlikely(!realpath(pgenname, g_textbuf)))
#endif
      {
        logerrprintfww(kErrprintfFopen, pgenname, strerror(errno));
        goto Plink2Core_ret_OPEN_FAIL;
      }
      uint32_t pgen_rename = 0;
      if (make_plink2_flags & kfMakePgen) {
        snprintf(outname_end, kMaxOutfnameExtBlen, ".pgen");
        pgen_rename = RealpathIdentical(outname, g_textbuf, &(g_textbuf[kPglFnamesize + 64]));
      }
      if ((!pgen_rename) && ((make_plink2_flags & kfMakeBed) || (pcp->exportf_info.flags & kfExportfIndMajorBed))) {
        snprintf(outname_end, kMaxOutfnameExtBlen, ".bed");
        pgen_rename = RealpathIdentical(outname, g_textbuf, &(g_textbuf[kPglFnamesize + 64]));
      }
      if (pgen_rename) {
        logerrprintf("Warning: --make-%s input and output filenames match.  Appending '~' to input\nfilenames.\n", (make_plink2_flags & kfMakeBed)? "bed" : ((make_plink2_flags & kfMakePvar)? "pgen" : "bpgen"));
        fname_slen = strlen(pgenname);
        memcpy(g_textbuf, pgenname, fname_slen + 1);
        snprintf(&(pgenname[fname_slen]), 2, "~");
        if (unlikely(rename(g_textbuf, pgenname))) {
          logerrputs("Error: Failed to append '~' to input .bed/.pgen filename.\n");
          goto Plink2Core_ret_OPEN_FAIL;
        }
        fname_slen = strlen(pvarname);
        memcpy(g_textbuf, pvarname, fname_slen + 1);
        snprintf(&(pvarname[fname_slen]), 2, "~");
        if (unlikely(rename(g_textbuf, pvarname))) {
          logerrputs("Error: Failed to append '~' to input .bim/.pvar filename.\n");
          goto Plink2Core_ret_OPEN_FAIL;
        }
        pvar_renamed = 1;
        fname_slen = strlen(psamname);
        memcpy(g_textbuf, psamname, fname_slen + 1);
        snprintf(&(psamname[fname_slen]), 2, "~");
        if (unlikely(rename(g_textbuf, psamname))) {
          logerrputs("Error: Failed to append '~' to input .fam/.psam filename.\n");
          goto Plink2Core_ret_OPEN_FAIL;
        }
      }
    }
    uint32_t raw_sample_ct = 0;
    uintptr_t* sample_include = nullptr;
    uintptr_t* sex_nm = nullptr;
    uintptr_t* sex_male = nullptr;
    uintptr_t* founder_info = nullptr;
    uintptr_t max_pheno_name_blen = 0;
    uint32_t raw_sample_ctl = 0;
    uint32_t sample_ct = 0;

    uint32_t update_sample_ids_empty = 0;
    uint32_t update_parental_ids_empty = 0;

    // There's a tradeoff between using structs like this and passing each of
    // the components separately: large structs make for shorter parameter
    // lists and hence slightly prettier code, but going too far in that
    // direction (e.g. having a single huge struct that gets passed to all
    // commands) makes it far harder to see what the actual data dependencies
    // are.  Right now the codebase errs, to an almost comical degree, in the
    // direction of making data dependencies clear and minimizing passing of
    // unneeded information.  I'll scale this back, but I'll try to avoid doing
    // it in ways that obscure data flows.
    // For instance, sample IDs and parental IDs could be stored in the same
    // struct, but if we pass parental IDs separately it's easier to see which
    // functions actually need them.
    // In lower-level functions, a reasonable rule of thumb is that aggregation
    // of parameters that normally appear together is worthwhile when there are
    // more than 6 parameters, since the x86-64 C calling convention uses
    // registers for the first 6 parameters.
    PedigreeIdInfo pii;
    InitPedigreeIdInfo(pcp->misc_flags, &pii);
    if (psamname[0]) {
      if (pcp->update_sample_ids_fname) {
        reterr = PrescanSampleIds(pcp->update_sample_ids_fname, &pii.sii);
        if (reterr) {
          if (unlikely(reterr != kPglRetEof)) {
            goto Plink2Core_ret_1;
          }
          reterr = kPglRetSuccess;
          update_sample_ids_empty = 1;
        }
      }
      if (pcp->update_parental_ids_fname) {
        reterr = PrescanParentalIds(pcp->update_parental_ids_fname, pcp->max_thread_ct, &pii.parental_id_info);
        if (reterr) {
          if (unlikely(reterr != kPglRetEof)) {
            goto Plink2Core_ret_1;
          }
          reterr = kPglRetSuccess;
          update_parental_ids_empty = 1;
        }
      }
      // update (26 Nov 2017): change --no-pheno to also apply to .psam file
      const uint32_t ignore_psam_phenos = (!(pcp->fam_cols & kfFamCol6)) || (pcp->pheno_fname && pcp->pheno_range_list.name_ct);
      reterr = LoadPsam(psamname, pcp->pheno_fname? nullptr : &(pcp->pheno_range_list), pcp->fam_cols, ignore_psam_phenos? 0 : 0x7fffffff, pcp->missing_pheno, (pcp->misc_flags / kfMiscAffection01) & 1, pcp->max_thread_ct, &pii, &sample_include, &founder_info, &sex_nm, &sex_male, &pheno_cols, &pheno_names, &raw_sample_ct, &pheno_ct, &max_pheno_name_blen);
      if (unlikely(reterr)) {
        goto Plink2Core_ret_1;
      }
      // todo: move this check after --update-ids once that's implemented.
      if ((pii.sii.flags & kfSampleIdFidPresent) && ((pii.sii.flags & kfSampleIdNoIdHeaderIidOnly) || (pcp->grm_flags & kfGrmNoIdHeaderIidOnly))) {
        for (uint32_t sample_idx = 0; sample_idx != raw_sample_ct; ++sample_idx) {
          const char* cur_sample_id = &(pii.sii.sample_ids[sample_idx * pii.sii.max_sample_id_blen]);
          if (unlikely(!memequal_k(cur_sample_id, "0\t", 2))) {
            logerrputs("Error: 'iid-only' modifier can only be used when FIDs are missing or all-0.\n");
            goto Plink2Core_ret_INCONSISTENT_INPUT;
          }
        }
      }

      // todo: add option to discard loaded SIDs
      raw_sample_ctl = BitCtToWordCt(raw_sample_ct);
      sample_ct = PopcountWords(sample_include, raw_sample_ctl);
      const uint32_t known_sex_ct = PopcountWords(sex_nm, raw_sample_ctl);
      const uint32_t male_ct = PopcountWords(sex_male, raw_sample_ctl);
      const uint32_t female_ct = known_sex_ct - male_ct;
      const uint32_t founder_ct = PopcountWords(founder_info, raw_sample_ctl);
      if (known_sex_ct == sample_ct) {
        logprintfww("%u sample%s (%u female%s, %u male%s; %u founder%s) loaded from %s.\n", sample_ct, (sample_ct == 1)? "" : "s", female_ct, (female_ct == 1)? "" : "s", male_ct, (male_ct == 1)? "" : "s", founder_ct, (founder_ct == 1)? "" : "s", psamname);
      } else {
        const uint32_t unknown_sex_ct = sample_ct - known_sex_ct;
        logprintfww("%u sample%s (%u female%s, %u male%s, %u ambiguous; %u founder%s) loaded from %s.\n", sample_ct, (sample_ct == 1)? "" : "s", female_ct, (female_ct == 1)? "" : "s", male_ct, (male_ct == 1)? "" : "s", unknown_sex_ct, founder_ct, (founder_ct == 1)? "" : "s", psamname);
      }
    }

    uint32_t max_variant_id_slen = 1;
    uint32_t info_reload_slen = InfoReloadIsNeeded(pcp->command_flags1, pcp->pvar_psam_flags, pcp->exportf_info.flags, pcp->rmdup_mode);
    uintptr_t* allele_idx_offsets = nullptr;
    uint32_t raw_variant_ct = 0;
    uint32_t variant_ct = 0;
    char* xheader = nullptr;
    uintptr_t xheader_blen = 0;
    uintptr_t* variant_include = nullptr;
    uint32_t* variant_bps = nullptr;
    char** variant_ids_mutable = nullptr;

    // This is actually a hybrid of const char** and char**: length >1
    // pointed-to strings can be modified in-place by --update-alleles, but we
    // must be able to assign const pointers to length-1 strings to it.  So we
    // set the official type to char**, but there's more const_cast-ing than
    // usual.
    char** allele_storage_mutable = nullptr;

    uintptr_t* pvar_qual_present = nullptr;
    float* pvar_quals = nullptr;
    uintptr_t* pvar_filter_present = nullptr;
    uintptr_t* pvar_filter_npass = nullptr;
    const char* const* pvar_filter_storage = nullptr;
    uintptr_t* nonref_flags = nullptr;
    InfoFlags info_flags = kfInfo0;
    uint32_t max_allele_ct = 2;
    uint32_t max_allele_slen = 0;
    uint32_t max_filter_slen = 0;
    UnsortedVar vpos_sortstatus = kfUnsortedVar0;
    double* variant_cms = nullptr;
    ChrIdx* chr_idxs = nullptr;  // split-chromosome case only
    if (pvarname[0]) {
      char** pvar_filter_storage_mutable = nullptr;

      // LoadPvar() uses pvar_psam_flags to determine what's needed for .pvar
      // export.  These booleans are just for tracking requirements beyond
      // that.
      const uint32_t xheader_needed = (pcp->exportf_info.flags & kfExportfVcf)? 1 : 0;
      const uint32_t qualfilter_needed = xheader_needed || ((pcp->rmdup_mode != kRmDup0) && (pcp->rmdup_mode <= kRmDupExcludeMismatch));

      reterr = LoadPvar(pvarname, pcp->var_filter_exceptions_flattened, pcp->varid_template_str, pcp->varid_multi_template_str, pcp->varid_multi_nonsnp_template_str, pcp->missing_varid_match, pcp->require_info_flattened, pcp->require_no_info_flattened, &(pcp->extract_if_info_expr), &(pcp->exclude_if_info_expr), pcp->misc_flags, pcp->pvar_psam_flags, xheader_needed, qualfilter_needed, pcp->var_min_qual, pcp->splitpar_bound1, pcp->splitpar_bound2, pcp->new_variant_id_max_allele_slen, (pcp->filter_flags / kfFilterSnpsOnly) & 3, !(pcp->dependency_flags & kfFilterNoSplitChr), pcp->filter_min_allele_ct, pcp->filter_max_allele_ct, pcp->max_thread_ct, cip, &max_variant_id_slen, &info_reload_slen, &vpos_sortstatus, &xheader, &variant_include, &variant_bps, &variant_ids_mutable, &allele_idx_offsets, K_CAST(const char***, &allele_storage_mutable), &pvar_qual_present, &pvar_quals, &pvar_filter_present, &pvar_filter_npass, &pvar_filter_storage_mutable, &nonref_flags, &variant_cms, &chr_idxs, &raw_variant_ct, &variant_ct, &max_allele_ct, &max_allele_slen, &xheader_blen, &info_flags, &max_filter_slen);
      if (unlikely(reterr)) {
        goto Plink2Core_ret_1;
      }
      if (unlikely(!variant_ct)) {
        // conditionally permit this?
        if (raw_variant_ct) {
          logerrprintfww("Error: No variants loaded from %s.\n", pvarname);
          goto Plink2Core_ret_INCONSISTENT_INPUT;
        }
        logerrprintfww("Error: No variants in %s.\n", pvarname);
        goto Plink2Core_ret_MALFORMED_INPUT;
      }
      pvar_filter_storage = TO_CONSTCPCONSTP(pvar_filter_storage_mutable);
      if (variant_ct == raw_variant_ct) {
        logprintfww("%u variant%s loaded from %s.\n", variant_ct, (variant_ct == 1)? "" : "s", pvarname);
      } else {
        logprintfww("%u out of %u variant%s loaded from %s.\n", variant_ct, raw_variant_ct, (raw_variant_ct == 1)? "" : "s", pvarname);
      }
      if (info_reload_slen && (make_plink2_flags & (kfMakeBim | kfMakePvar)) && (!pvar_renamed)) {
        // need to be careful with .pvar in this case
        uint32_t fname_slen;
#ifdef _WIN32
        fname_slen = GetFullPathName(pvarname, kPglFnamesize, g_textbuf, nullptr);
        if (unlikely((!fname_slen) || (fname_slen > kPglFnamesize)))
#else
        if (unlikely(!realpath(pvarname, g_textbuf)))
#endif
        {
          logerrprintfww(kErrprintfFopen, pvarname, strerror(errno));
          goto Plink2Core_ret_OPEN_FAIL;
        }
        if (make_plink2_flags & kfMakeBim) {
          OutnameZstSet(".bim", make_plink2_flags & kfMakeBimZs, outname_end);
          pvar_renamed = RealpathIdentical(outname, g_textbuf, &(g_textbuf[kPglFnamesize + 64]));
          if (pvar_renamed) {
            logerrputs("Warning: .bim input and output filenames match.  Appending '~' to input\nfilename.\n");
            fname_slen = strlen(pvarname);
            memcpy(g_textbuf, pvarname, fname_slen + 1);
            snprintf(&(pvarname[fname_slen]), 2, "~");
            if (unlikely(rename(g_textbuf, pvarname))) {
              logerrputs("Error: Failed to append '~' to input .bim filename.\n");
              goto Plink2Core_ret_OPEN_FAIL;
            }
          }
        }
        if ((!pvar_renamed) && (make_plink2_flags & kfMakePvar)) {
          OutnameZstSet(".pvar", pcp->pvar_psam_flags & kfPvarZs, outname_end);
          // pvar_renamed = RealpathIdentical();
          if (RealpathIdentical(outname, g_textbuf, &(g_textbuf[kPglFnamesize + 64]))) {
            logerrputs("Warning: .pvar input and output filenames match.  Appending '~' to input\nfilename.\n");
            fname_slen = strlen(pvarname);
            memcpy(g_textbuf, pvarname, fname_slen + 1);
            snprintf(&(pvarname[fname_slen]), 2, "~");
            if (unlikely(rename(g_textbuf, pvarname))) {
              logerrputs("Error: Failed to append '~' to input .pvar filename.\n");
              goto Plink2Core_ret_OPEN_FAIL;
            }
          }
        }
      }
      if ((pcp->dependency_flags & (kfFilterAllReq | kfFilterNonrefFlagsNeeded)) == kfFilterNonrefFlagsNeeded) {
        // We need INFO:PR, but we don't really care about anything else
        // potentially in the .pgen.
        // So if INFO was present in the variant file, we don't need to look at
        // the .pgen at all (even if there was no PR key; we infer that all
        // reference alleles are accurate in that case).
        // Conversely, if no .pgen filename was provided, and the variant file
        // doesn't contain an INFO field, print a warning.
        if (info_flags & kfInfoAll) {
          pgenname[0] = '\0';
        } else if (!pgenname[0]) {
          logerrputs("Warning: Variant file does not distinguish between provisional and trusted REF\nalleles, and no .pgen input file was provided.\n");
          if (info_flags & kfInfoPrNonrefDefault) {
            logerrputs("Assuming all REF alleles are provisional.\n");
          } else {
            logerrputs("Assuming all REF alleles are trusted.\n");
          }
        }
      }
    }

    const uint32_t raw_variant_ctl = BitCtToWordCt(raw_variant_ct);
    uintptr_t pgr_alloc_cacheline_ct = 0;
    if (pgenname[0]) {
      PgenHeaderCtrl header_ctrl;
      uintptr_t cur_alloc_cacheline_ct;
      while (1) {
        reterr = PgfiInitPhase1(pgenname, raw_variant_ct, raw_sample_ct, 0, &header_ctrl, &pgfi, &cur_alloc_cacheline_ct, g_logbuf);
        if (!reterr) {
          break;
        }
        // detect and autoconvert plink 1 sample-major files, instead of
        // failing (don't bother supporting plink 0.99 files any more)
        if (unlikely(reterr != kPglRetSampleMajorBed)) {
          WordWrapB(0);
          logerrputsb();
          goto Plink2Core_ret_1;
        }
        char* pgenname_end = memcpya(pgenname, outname, outname_end - outname);
        pgenname_end = strcpya_k(pgenname_end, ".pgen");
        const uint32_t no_vmaj_ext = (pcp->command_flags1 & kfCommand1MakePlink2) && (!pcp->filter_flags) && ((make_plink2_flags & (kfMakePgen | (kfMakePgenFormatBase * 3))) == kfMakePgen);
        if (no_vmaj_ext) {
          *pgenname_end = '\0';
          make_plink2_flags &= ~kfMakePgen;
          // no --make-just-pgen command, so we'll never entirely skip the
          // make_plink2 operation
        } else {
          snprintf(pgenname_end, kMaxOutfnameExtBlen - 5, ".vmaj");
        }
        reterr = Plink1SampleMajorToPgen(pgenname, raw_variant_ct, raw_sample_ct, (pcp->misc_flags / kfMiscRealRefAlleles) & 1, pcp->max_thread_ct, pgfi.shared_ff);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
        fclose(pgfi.shared_ff);
        pgfi.shared_ff = nullptr;
      }
      pgfi.allele_idx_offsets = allele_idx_offsets;
      pgfi.max_allele_ct = max_allele_ct;
      unsigned char* pgfi_alloc;
      if (unlikely(bigstack_alloc_uc(cur_alloc_cacheline_ct * kCacheline, &pgfi_alloc))) {
        goto Plink2Core_ret_NOMEM;
      }
      const uint32_t nonref_flags_already_loaded = (nonref_flags != nullptr);
      if ((!nonref_flags) && ((header_ctrl & 192) == 192)) {
        if (unlikely(bigstack_alloc_w(raw_variant_ctl, &nonref_flags))) {
          goto Plink2Core_ret_NOMEM;
        }
      }
      pgfi.nonref_flags = nonref_flags;
      uint32_t max_vrec_width;
      // only practical effect of setting use_blockload to zero here is that
      // pgr_alloc_cacheline_ct is overestimated by
      // DivUp(max_vrec_width, kCacheline).
      reterr = PgfiInitPhase2(header_ctrl, 1, nonref_flags_already_loaded, 1, 0, raw_variant_ct, &max_vrec_width, &pgfi, pgfi_alloc, &pgr_alloc_cacheline_ct, g_logbuf);
      if (unlikely(reterr)) {
        WordWrapB(0);
        logerrputsb();
        goto Plink2Core_ret_1;
      }
      if (unlikely((!allele_idx_offsets) && (pgfi.gflags & kfPgenGlobalMultiallelicHardcallFound))) {
        logerrputs("Error: .pgen file contains multiallelic variants, while .pvar does not.\n");
        goto Plink2Core_ret_INCONSISTENT_INPUT;
      }
      if (pcp->misc_flags & kfMiscRealRefAlleles) {
        if (unlikely(nonref_flags && (!AllBitsAreOne(nonref_flags, raw_variant_ct)))) {
          // technically a lie, it's okay if a .bed is first converted to .pgen
          // without this flag, and then the user remembers the existence of
          // --real-ref-alleles later.  but to reduce the ease of
          // foot-shooting, we don't allow this to clobber arbitrary
          // nonref_flags arrays.
          logerrputs("Error: --real-ref-alleles must be used on a plink1 fileset.\n");
          goto Plink2Core_ret_INCONSISTENT_INPUT;
        }

        // wasteful if nonref_flags was allocated, but nonref_flags isn't that
        // large, and --real-ref-alleles + --make-pgen can be run separately
        // from anything truly memory-limited
        nonref_flags = nullptr;
        pgfi.nonref_flags = nullptr;

        pgfi.gflags &= ~kfPgenGlobalAllNonref;
      }
      if (SingleVariantLoaderIsNeeded(king_cutoff_fprefix, pcp->command_flags1, make_plink2_flags, pcp->rmdup_mode, pcp->hwe_thresh)) {
        // ugly kludge, probably want to add pgenlib_internal support for this
        // hybrid use pattern
        FILE* shared_ff_copy = pgfi.shared_ff;
        pgfi.shared_ff = nullptr;
        unsigned char* simple_pgr_alloc;
        if (unlikely(bigstack_alloc_uc((pgr_alloc_cacheline_ct + DivUp(max_vrec_width, kCacheline)) * kCacheline, &simple_pgr_alloc))) {
          goto Plink2Core_ret_NOMEM;
        }
        reterr = PgrInit(pgenname, max_vrec_width, &pgfi, &simple_pgr, simple_pgr_alloc);
        if (unlikely(reterr)) {
          if (reterr == kPglRetOpenFail) {
            logerrprintfww(kErrprintfFopen, pgenname, strerror(errno));
          } else {
            assert(reterr == kPglRetReadFail);
            logerrprintfww(kErrprintfFread, pgenname, strerror(errno));
          }
          goto Plink2Core_ret_1;
        }
        pgfi.shared_ff = shared_ff_copy;
        // todo: make this execute after --pmerge
        if (pcp->command_flags1 & kfCommand1Validate) {
          uintptr_t* genovec_buf;
          if (unlikely(bigstack_alloc_w(NypCtToWordCt(raw_sample_ct), &genovec_buf))) {
            goto Plink2Core_ret_NOMEM;
          }
          logprintfww5("Validating %s... ", pgenname);
          fflush(stdout);
          reterr = PgrValidate(&simple_pgr, genovec_buf, g_logbuf);
          if (unlikely(reterr)) {
            logputs("\n");
            WordWrapB(0);
            logerrputsb();
            goto Plink2Core_ret_1;
          }
          logputs("done.\n");
          if (pcp->command_flags1 == kfCommand1Validate) {
            goto Plink2Core_ret_1;
          }
          BigstackReset(genovec_buf);
        }
      }
      // any functions using blockload must perform its own PgrInit(), etc.
      if (pcp->command_flags1 & kfCommand1PgenInfo) {
        if (pgfi.const_vrtype == kPglVrtypePlink1) {
          logerrputs("Warning: Skipping --pgen-info since a .bed file was provided.\n");
        } else {
          PgenInfoPrint(pgenname, &pgfi, header_ctrl, max_allele_ct);
        }
        if (!(pcp->command_flags1 & (~(kfCommand1Validate | kfCommand1PgenInfo)))) {
          goto Plink2Core_ret_1;
        }
      }

    } else {
      // bugfix (10-11 Feb 2018): these variables may still be accessed
      pgfi.gflags = S_CAST(PgenGlobalFlags, ((info_flags / kfInfoPrNonrefDefault) & 1) * kfPgenGlobalAllNonref);
      pgfi.nonref_flags = nonref_flags;
    }
    if (pcp->pheno_fname) {
      reterr = LoadPhenos(pcp->pheno_fname, &(pcp->pheno_range_list), sample_include, pii.sii.sample_ids, raw_sample_ct, sample_ct, pii.sii.max_sample_id_blen, pcp->missing_pheno, (pcp->misc_flags / kfMiscAffection01) & 1, (pcp->misc_flags / kfMiscPhenoIidOnly) & 1, (pcp->misc_flags / kfMiscPhenoColNums) & 1, pcp->max_thread_ct, &pheno_cols, &pheno_names, &pheno_ct, &max_pheno_name_blen);
      if (unlikely(reterr)) {
        goto Plink2Core_ret_1;
      }
    }

    // move processing of PLINK 1.x cluster-loading/filtering flags here, since
    // they're now under the categorical-phenotype umbrella
    if ((pcp->misc_flags & kfMiscCatPhenoFamily) || pcp->within_fname) {
      reterr = Plink1ClusterImport(pcp->within_fname, pcp->catpheno_name, pcp->family_missing_catname, sample_include, pii.sii.sample_ids, raw_sample_ct, sample_ct, pii.sii.max_sample_id_blen, pcp->mwithin_val, pcp->max_thread_ct, &pheno_cols, &pheno_names, &pheno_ct, &max_pheno_name_blen);
      if (unlikely(reterr)) {
        goto Plink2Core_ret_1;
      }
    }

    if (!pheno_ct) {
      logputs("Note: No phenotype data present.\n");
    } else {
      if (pheno_ct == 1) {
        const uint32_t obs_ct = PopcountWords(pheno_cols[0].nonmiss, raw_sample_ctl);
        if (pheno_cols[0].type_code == kPhenoDtypeCc) {
          const uint32_t case_ct = PopcountWords(pheno_cols[0].data.cc, raw_sample_ctl);
          const uint32_t ctrl_ct = obs_ct - case_ct;
          logprintf("1 binary phenotype loaded (%u case%s, %u control%s).\n", case_ct, (case_ct == 1)? "" : "s", ctrl_ct, (ctrl_ct == 1)? "" : "s");
        } else if (pheno_cols[0].type_code == kPhenoDtypeQt) {
          logprintf("1 quantitative phenotype loaded (%u value%s).\n", obs_ct, (obs_ct == 1)? "" : "s");
        } else {
          logprintf("1 categorical phenotype loaded (%u value%s).\n", obs_ct, (obs_ct == 1)? "" : "s");
        }
      } else {
        uint32_t cc_ct = 0;
        uint32_t qt_ct = 0;
        for (uint32_t pheno_idx = 0; pheno_idx != pheno_ct; ++pheno_idx) {
          const PhenoDtype cur_type_code = pheno_cols[pheno_idx].type_code;
          if (pheno_cols[pheno_idx].type_code == kPhenoDtypeCc) {
            ++cc_ct;
          } else if (cur_type_code == kPhenoDtypeQt) {
            ++qt_ct;
          }
        }
        uint32_t cat_ct = pheno_ct - cc_ct - qt_ct;
        // just brute force this for now
        if (!cc_ct) {
          if (!qt_ct) {
            logprintf("%u categorical phenotypes loaded.\n", pheno_ct);
          } else if (!cat_ct) {
            logprintf("%u quantitative phenotypes loaded.\n", pheno_ct);
          } else {
            logprintf("%u phenotypes loaded (%u quantitative, %u categorical).\n", pheno_ct, qt_ct, cat_ct);
          }
        } else if (!qt_ct) {
          if (!cat_ct) {
            logprintf("%u binary phenotypes loaded.\n", pheno_ct);
          } else {
            logprintf("%u phenotypes loaded (%u binary, %u categorical).\n", pheno_ct, cc_ct, cat_ct);
          }
        } else if (!cat_ct) {
          logprintf("%u phenotypes loaded (%u binary, %u quantitative).\n", pheno_ct, cc_ct, qt_ct);
        } else {
          logprintfww("%u phenotypes loaded (%u binary, %u quantitative, %u categorical).\n", pheno_ct, cc_ct, qt_ct, cat_ct);
        }
      }
    }
    // If something like --snps is combined with a position-based filter which
    // may remove some of the named variants, we need to apply --snps first.
    // Otherwise, it may be very advantageous to apply the position-based
    // filters before constructing the variant ID hash table.  So we split this
    // into two cases.
    // (Actually, hash table construction is now usually fast enough, even with
    // >80m variants, that this optimization isn't a big deal.  But it's
    // already implemented, so let's keep it around until/unless it creates
    // some sort of conflict.)

    // Additional minor optimization: --recover-var-ids doesn't actually need
    // the ID hash table; it's just positioned in this code block anyway
    // because it would be too weird for it to be in a different position than
    // --update-name in the order of operations.
    const uint32_t htable_needed_early = variant_ct && (pcp->varid_from || pcp->varid_to || pcp->varid_snp || pcp->varid_exclude_snp || pcp->snps_range_list.name_ct || pcp->exclude_snps_range_list.name_ct);
    const uint32_t full_variant_id_htable_needed = variant_ct && (htable_needed_early || pcp->update_map_flag || pcp->update_name_flag || pcp->update_alleles_fname || (pcp->rmdup_mode != kRmDup0) || pcp->extract_fcol_info.params);
    if (!full_variant_id_htable_needed) {
      reterr = ApplyVariantBpFilters(pcp->extract_fnames, pcp->extract_intersect_fnames, pcp->exclude_fnames, cip, variant_bps, pcp->from_bp, pcp->to_bp, raw_variant_ct, pcp->filter_flags, vpos_sortstatus, pcp->max_thread_ct, variant_include, &variant_ct);
      if (unlikely(reterr)) {
        goto Plink2Core_ret_1;
      }
    }
    const uint32_t extract_exclude_by_id = (pcp->extract_fnames && (!(pcp->filter_flags & (kfFilterExtractBed0 | kfFilterExtractBed1)))) || (pcp->extract_intersect_fnames && (!(pcp->filter_flags & (kfFilterExtractIntersectBed0 | kfFilterExtractIntersectBed1)))) || (pcp->exclude_fnames && (!(pcp->filter_flags & (kfFilterExcludeBed0 | kfFilterExcludeBed1))));
    if (variant_ct && (full_variant_id_htable_needed || extract_exclude_by_id || pcp->recover_var_ids_fname)) {
      if (vpos_sortstatus & kfUnsortedVarBp) {
        if (unlikely(pcp->varid_from || pcp->varid_to)) {
          logerrputs("Error: --from/--to require a sorted .pvar/.bim.  Retry this command after using\n--make-pgen/--make-bed + --sort-vars to sort your data.\n");
          goto Plink2Core_ret_INCONSISTENT_INPUT;
        }
        if (unlikely(pcp->window_bp != -1)) {
          logerrputs("Error: --window requires a sorted .pvar/.bim.  Retry this command after using\n--make-pgen/--make-bed + --sort-vars to sort your data.\n");
          goto Plink2Core_ret_INCONSISTENT_INPUT;
        }
        if (unlikely(pcp->recover_var_ids_fname)) {
          logerrputs("Error: --recover-var-ids requires a sorted .pvar/.bim.  Retry this command\nafter using --make-pgen/--make-bed + --sort-vars to sort your data.\n");
          goto Plink2Core_ret_INCONSISTENT_INPUT;
        }
      }
      // don't bother with having different allow_dups vs. no allow_dups hash
      // table structures, just check specific IDs for duplication in the
      // no-duplicates-allowed cases
      unsigned char* bigstack_mark = g_bigstack_base;
      uint32_t* variant_id_htable = nullptr;
      uint32_t* htable_dup_base = nullptr;
      uint32_t dup_ct = 0;
      uint32_t variant_id_htable_size = 0;
      if ((!pcp->recover_var_ids_fname) || htable_needed_early) {
        reterr = AllocAndPopulateIdHtableMt(variant_include, TO_CONSTCPCONSTP(variant_ids_mutable), variant_ct, bigstack_left() / 8, pcp->max_thread_ct, &variant_id_htable, &htable_dup_base, &variant_id_htable_size, &dup_ct);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
      }
      if (pcp->varid_from || pcp->varid_to) {
        reterr = FromToFlag(TO_CONSTCPCONSTP(variant_ids_mutable), variant_id_htable, htable_dup_base, pcp->varid_from, pcp->varid_to, raw_variant_ct, max_variant_id_slen, variant_id_htable_size, variant_include, cip, &variant_ct);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
      }
      if (pcp->varid_snp) {
        reterr = SnpFlag(variant_bps, TO_CONSTCPCONSTP(variant_ids_mutable), variant_id_htable, htable_dup_base, pcp->varid_snp, raw_variant_ct, max_variant_id_slen, variant_id_htable_size, 0, pcp->window_bp, variant_include, cip, &variant_ct);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
      }
      if (pcp->snps_range_list.name_ct) {
        reterr = SnpsFlag(TO_CONSTCPCONSTP(variant_ids_mutable), variant_id_htable, htable_dup_base, &(pcp->snps_range_list), raw_variant_ct, max_variant_id_slen, variant_id_htable_size, 0, variant_include, &variant_ct);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
      }
      if (pcp->varid_exclude_snp) {
        reterr = SnpFlag(variant_bps, TO_CONSTCPCONSTP(variant_ids_mutable), variant_id_htable, htable_dup_base, pcp->varid_exclude_snp, raw_variant_ct, max_variant_id_slen, variant_id_htable_size, 1, pcp->window_bp, variant_include, cip, &variant_ct);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
      }
      if (pcp->exclude_snps_range_list.name_ct) {
        reterr = SnpsFlag(TO_CONSTCPCONSTP(variant_ids_mutable), variant_id_htable, htable_dup_base, &(pcp->exclude_snps_range_list), raw_variant_ct, max_variant_id_slen, variant_id_htable_size, 1, variant_include, &variant_ct);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
      }

      if (variant_ct) {
        if (pcp->update_map_flag) {
          reterr = UpdateVarBps(cip, TO_CONSTCPCONSTP(variant_ids_mutable), variant_id_htable, htable_dup_base, pcp->update_map_flag, raw_variant_ct, max_variant_id_slen, variant_id_htable_size, pcp->max_thread_ct, variant_include, variant_bps, &variant_ct, &vpos_sortstatus);
          if (unlikely(reterr)) {
            goto Plink2Core_ret_1;
          }
        } else if (pcp->update_name_flag || pcp->recover_var_ids_fname) {
          if (pcp->update_name_flag) {
            reterr = UpdateVarNames(variant_include, variant_id_htable, htable_dup_base, pcp->update_name_flag, raw_variant_ct, variant_id_htable_size, pcp->max_thread_ct, variant_ids_mutable, &max_variant_id_slen);
          } else {
            reterr = RecoverVarIds(pcp->recover_var_ids_fname, variant_include, cip, variant_bps, allele_idx_offsets, TO_CONSTCPCONSTP(allele_storage_mutable), pcp->missing_varid_match, raw_variant_ct, variant_ct, pcp->recover_var_ids_flags, pcp->max_thread_ct, variant_ids_mutable, &max_variant_id_slen, outname, outname_end);
          }
          if (unlikely(reterr)) {
            goto Plink2Core_ret_1;
          }
          if (extract_exclude_by_id || pcp->update_alleles_fname || (pcp->rmdup_mode != kRmDup0)) {
            // Must (re)construct the hash table in this case.
            BigstackReset(bigstack_mark);
            dup_ct = 0;
            reterr = AllocAndPopulateIdHtableMt(variant_include, TO_CONSTCPCONSTP(variant_ids_mutable), variant_ct, bigstack_left() / 8, pcp->max_thread_ct, &variant_id_htable, &htable_dup_base, &variant_id_htable_size, &dup_ct);
            if (unlikely(reterr)) {
              goto Plink2Core_ret_1;
            }
          }
        }

        if (pcp->update_alleles_fname) {
          reterr = UpdateVarAlleles(pcp->update_alleles_fname, variant_include, TO_CONSTCPCONSTP(variant_ids_mutable), variant_id_htable, htable_dup_base, allele_idx_offsets, raw_variant_ct, max_variant_id_slen, variant_id_htable_size, pcp->max_thread_ct, allele_storage_mutable, &max_allele_slen, outname, outname_end);
          if (unlikely(reterr)) {
            goto Plink2Core_ret_1;
          }
        }

        if (pcp->extract_fnames && (!(pcp->filter_flags & (kfFilterExtractBed0 | kfFilterExtractBed1)))) {
          reterr = ExtractExcludeFlagNorange(TO_CONSTCPCONSTP(variant_ids_mutable), variant_id_htable, htable_dup_base, pcp->extract_fnames, raw_variant_ct, max_variant_id_slen, variant_id_htable_size, kVfilterExtract, pcp->max_thread_ct, variant_include, &variant_ct);
          if (unlikely(reterr)) {
            goto Plink2Core_ret_1;
          }
        }
        if (pcp->extract_intersect_fnames && (!(pcp->filter_flags & (kfFilterExtractIntersectBed0 | kfFilterExtractIntersectBed1)))) {
          reterr = ExtractExcludeFlagNorange(TO_CONSTCPCONSTP(variant_ids_mutable), variant_id_htable, htable_dup_base, pcp->extract_intersect_fnames, raw_variant_ct, max_variant_id_slen, variant_id_htable_size, kVfilterExtractIntersect, pcp->max_thread_ct, variant_include, &variant_ct);
          if (unlikely(reterr)) {
            goto Plink2Core_ret_1;
          }
        }
        if (pcp->exclude_fnames && (!(pcp->filter_flags & (kfFilterExcludeBed0 | kfFilterExcludeBed1)))) {
          reterr = ExtractExcludeFlagNorange(TO_CONSTCPCONSTP(variant_ids_mutable), variant_id_htable, htable_dup_base, pcp->exclude_fnames, raw_variant_ct, max_variant_id_slen, variant_id_htable_size, kVfilterExclude, pcp->max_thread_ct, variant_include, &variant_ct);
          if (unlikely(reterr)) {
            goto Plink2Core_ret_1;
          }
        }
        if (pcp->extract_fcol_info.params) {
          reterr = ExtractFcol(TO_CONSTCPCONSTP(variant_ids_mutable), variant_id_htable, htable_dup_base, &pcp->extract_fcol_info, raw_variant_ct, max_variant_id_slen, variant_id_htable_size, pcp->max_thread_ct, variant_include, &variant_ct);
          if (unlikely(reterr)) {
            goto Plink2Core_ret_1;
          }
        }
        if (pcp->rmdup_mode != kRmDup0) {
          reterr = RmDup(sample_include, cip, variant_bps, TO_CONSTCPCONSTP(variant_ids_mutable), variant_id_htable, htable_dup_base, allele_idx_offsets, TO_CONSTCPCONSTP(allele_storage_mutable), pvar_qual_present, pvar_quals, pvar_filter_present, pvar_filter_npass, pvar_filter_storage, info_reload_slen? pvarname : nullptr, variant_cms, pcp->missing_varid_match, raw_sample_ct, sample_ct, raw_variant_ct, max_variant_id_slen, variant_id_htable_size, dup_ct, pcp->rmdup_mode, (pcp->command_flags1 / kfCommand1RmDupList) & 1, pcp->max_thread_ct, pgenname[0]? (&simple_pgr) : nullptr, variant_include, &variant_ct, outname, outname_end);
          if (reterr || (!(pcp->command_flags1 & (~(kfCommand1Validate | kfCommand1PgenInfo | kfCommand1RmDupList))))) {
            goto Plink2Core_ret_1;
          }
        }
      }

      BigstackReset(bigstack_mark);
      if (full_variant_id_htable_needed) {
        reterr = ApplyVariantBpFilters(pcp->extract_fnames, pcp->extract_intersect_fnames, pcp->exclude_fnames, cip, variant_bps, pcp->from_bp, pcp->to_bp, raw_variant_ct, pcp->filter_flags, vpos_sortstatus, pcp->max_thread_ct, variant_include, &variant_ct);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
      }

      // todo: --attrib; although it isn't a "standard" file format, it can be
      // more convenient than forcing users to generate full-blown sites-only
      // VCF files, etc.
    }
    // variant_ids[] is fixed from this point on.
    const char* const* variant_ids = TO_CONSTCPCONSTP(variant_ids_mutable);
    // SetRefalt1FromFile() can alter pointers-to-missing in
    // allele_storage[] so we can't make this a const char* const*, but nothing
    // alters the pointed-to strings after this point.
    const char** allele_storage = K_CAST(const char**, allele_storage_mutable);

    if (pcp->thin_keep_prob != 1.0) {
      RandomThinProb("thin", "variant", pcp->thin_keep_prob, raw_variant_ct, sfmtp, variant_include, &variant_ct);
    } else if (pcp->thin_keep_ct != UINT32_MAX) {
      reterr = RandomThinCt("thin-count", "variant", pcp->thin_keep_ct, raw_variant_ct, sfmtp, variant_include, &variant_ct);
      if (unlikely(reterr)) {
        goto Plink2Core_ret_1;
      }
    }
    uint32_t* sample_missing_dosage_cts = nullptr;
    uint32_t* sample_missing_hc_cts = nullptr;
    uint32_t* sample_hethap_cts = nullptr;
    uintptr_t max_covar_name_blen = 0;
    if (psamname[0]) {
      // xid_mode may vary between these operations in a single run, and
      // sample-sort is relatively cheap, so we abandon plink 1.9's "construct
      // sample ID map only once" optimization.
      if (pcp->update_sample_ids_fname) {
        if (update_sample_ids_empty) {
          logputs("--update-ids: 0 samples updated.\n");
        } else {
          reterr = UpdateSampleIds(pcp->update_sample_ids_fname, sample_include, raw_sample_ct, sample_ct, &pii.sii);
          if (unlikely(reterr)) {
            goto Plink2Core_ret_1;
          }
        }
      } else {
        if (pcp->update_parental_ids_fname) {
          if (update_parental_ids_empty) {
            logputs("--update-parents: 0 samples updated.\n");
          } else {
            reterr = UpdateSampleParents(pcp->update_parental_ids_fname, &pii.sii, sample_include, raw_sample_ct, sample_ct, pcp->max_thread_ct, &pii.parental_id_info, founder_info);
            if (unlikely(reterr)) {
              goto Plink2Core_ret_1;
            }
          }
        }
        // --update-parents goes here
        if (pcp->update_sex_info.fname) {
          reterr = UpdateSampleSexes(sample_include, &pii.sii, &(pcp->update_sex_info), raw_sample_ct, sample_ct, pcp->max_thread_ct, sex_nm, sex_male);
          if (unlikely(reterr)) {
            goto Plink2Core_ret_1;
          }
        }
      }
      if (pcp->keepfam_fnames) {
        reterr = KeepOrRemove(pcp->keepfam_fnames, &pii.sii, raw_sample_ct, kfKeepFam, sample_include, &sample_ct);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
      }
      if (pcp->keep_fnames) {
        reterr = KeepOrRemove(pcp->keep_fnames, &pii.sii, raw_sample_ct, kfKeep0, sample_include, &sample_ct);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
      }
      if (pcp->removefam_fnames) {
        reterr = KeepOrRemove(pcp->removefam_fnames, &pii.sii, raw_sample_ct, kfKeepRemove | kfKeepFam, sample_include, &sample_ct);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
      }
      if (pcp->remove_fnames) {
        reterr = KeepOrRemove(pcp->remove_fnames, &pii.sii, raw_sample_ct, kfKeepRemove, sample_include, &sample_ct);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
      }

      // todo: --attrib-indiv

      if (pcp->keep_fcol_fname) {
        reterr = KeepFcol(pcp->keep_fcol_fname, &pii.sii, pcp->keep_fcol_flattened, pcp->keep_fcol_name, raw_sample_ct, pcp->keep_fcol_num, sample_include, &sample_ct);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
      }
      if (pcp->misc_flags & kfMiscRequirePheno) {
        reterr = RequirePheno(pheno_cols, pheno_names, pcp->require_pheno_flattened, raw_sample_ct, pheno_ct, max_pheno_name_blen, 0, sample_include, &sample_ct);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
      }
      if (pcp->filter_flags & (kfFilterExclFemales | kfFilterExclMales | kfFilterExclNosex)) {
        if (pcp->filter_flags & kfFilterExclFemales) {
          for (uint32_t widx = 0; widx != raw_sample_ctl; ++widx) {
            sample_include[widx] &= (~sex_nm[widx]) | sex_male[widx];
          }
        }
        if (pcp->filter_flags & kfFilterExclMales) {
          BitvecInvmask(sex_male, raw_sample_ctl, sample_include);
        }
        if (pcp->filter_flags & kfFilterExclNosex) {
          BitvecAnd(sex_nm, raw_sample_ctl, sample_include);
        }
        const uint32_t old_sample_ct = sample_ct;
        sample_ct = PopcountWords(sample_include, raw_sample_ctl);
        const uint32_t removed_ct = old_sample_ct - sample_ct;
        logprintf("%u sample%s removed due to sex filter(s).\n", removed_ct, (removed_ct == 1)? "" : "s");
      }
      if (pcp->filter_flags & (kfFilterExclFounders | kfFilterExclNonfounders)) {
        const uint32_t keep_founders = (pcp->filter_flags / kfFilterExclNonfounders) & 1;
        if (keep_founders) {
          BitvecAnd(founder_info, raw_sample_ctl, sample_include);
        } else {
          BitvecInvmask(founder_info, raw_sample_ctl, sample_include);
        }
        const uint32_t old_sample_ct = sample_ct;
        sample_ct = PopcountWords(sample_include, raw_sample_ctl);
        const uint32_t removed_ct = old_sample_ct - sample_ct;
        logprintf("--keep-%sfounders: %u sample%s removed.\n", keep_founders? "" : "non", removed_ct, (removed_ct == 1)? "" : "s");
      }

      if (pcp->thin_keep_sample_prob != 1.0) {
        RandomThinProb("thin-indiv", "sample", pcp->thin_keep_sample_prob, raw_sample_ct, sfmtp, sample_include, &sample_ct);
      } else if (pcp->thin_keep_sample_ct != UINT32_MAX) {
        reterr = RandomThinCt("thin-indiv-count", "sample", pcp->thin_keep_sample_ct, raw_sample_ct, sfmtp, sample_include, &sample_ct);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
      }

      const uint32_t smaj_missing_geno_report_requested = (pcp->command_flags1 & kfCommand1MissingReport) && (!(pcp->missing_rpt_flags & kfMissingRptVariantOnly));
      if ((pcp->mind_thresh < 1.0) || smaj_missing_geno_report_requested) {
        if (unlikely(
                bigstack_alloc_u32(raw_sample_ct, &sample_missing_hc_cts) ||
                bigstack_alloc_u32(raw_sample_ct, &sample_hethap_cts))) {
          goto Plink2Core_ret_NOMEM;
        }
        if (SampleMissingDosageCtsAreNeeded(pcp->misc_flags, smaj_missing_geno_report_requested, pcp->mind_thresh, pcp->missing_rpt_flags)) {
          if (pgfi.gflags & kfPgenGlobalDosagePresent) {
            if (unlikely(bigstack_alloc_u32(raw_sample_ct, &sample_missing_dosage_cts))) {
              goto Plink2Core_ret_NOMEM;
            }
          } else {
            sample_missing_dosage_cts = sample_missing_hc_cts;
          }
        }
        // could avoid this call and make LoadAlleleAndGenoCounts() do
        // double duty with --missing?
        reterr = LoadSampleMissingCts(sex_male, variant_include, cip, raw_variant_ct, variant_ct, raw_sample_ct, pcp->max_thread_ct, pgr_alloc_cacheline_ct, &pgfi, sample_missing_hc_cts, (pgfi.gflags & kfPgenGlobalDosagePresent)? sample_missing_dosage_cts : nullptr, sample_hethap_cts);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
        if (pcp->mind_thresh < 1.0) {
          uint32_t variant_ct_y = 0;
          uint32_t y_code;
          if (XymtExists(cip, kChrOffsetY, &y_code)) {
            variant_ct_y = CountChrVariantsUnsafe(variant_include, cip, y_code);
          }
          reterr = MindFilter((pcp->misc_flags & kfMiscMindDosage)? sample_missing_dosage_cts : sample_missing_hc_cts, (pcp->misc_flags & kfMiscMindHhMissing)? sample_hethap_cts : nullptr, &pii.sii, raw_sample_ct, variant_ct, variant_ct_y, pcp->mind_thresh, sample_include, sex_male, &sample_ct, outname, outname_end);
          if (unlikely(reterr)) {
            goto Plink2Core_ret_1;
          }
        }
        if (!smaj_missing_geno_report_requested) {
          BigstackReset(sample_missing_hc_cts);
        }
        // this results in a small "memory leak" when a regular missingness
        // report is requested, not a big deal
      }
      if (pcp->covar_fname || pcp->covar_range_list.name_ct) {
        const char* cur_covar_fname = pcp->covar_fname? pcp->covar_fname : (pcp->pheno_fname? pcp->pheno_fname : psamname);
        reterr = LoadPhenos(cur_covar_fname, &(pcp->covar_range_list), sample_include, pii.sii.sample_ids, raw_sample_ct, sample_ct, pii.sii.max_sample_id_blen, pcp->missing_pheno, 2, (pcp->misc_flags / kfMiscCovarIidOnly) & 1, (pcp->misc_flags / kfMiscCovarColNums) & 1, pcp->max_thread_ct, &covar_cols, &covar_names, &covar_ct, &max_covar_name_blen);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
        logprintf("%u covariate%s loaded from %s.\n", covar_ct, (covar_ct == 1)? "" : "s", cur_covar_fname);

        // do we still want to clear some main phenotype values here if some
        // covariate values are missing?  (don't think there's a point to
        // preserving that behavior, let the regression functions do it to
        // their local phenotype copies on their own.)
      }

      if (pcp->misc_flags & kfMiscRequireCovar) {
        reterr = RequirePheno(covar_cols, covar_names, pcp->require_covar_flattened, raw_sample_ct, covar_ct, max_covar_name_blen, 1, sample_include, &sample_ct);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
      }
      if (pcp->keep_if_expr.pheno_name) {
        reterr = KeepRemoveIf(&(pcp->keep_if_expr), pheno_cols, pheno_names, covar_cols, covar_names, raw_sample_ct, pheno_ct, max_pheno_name_blen, covar_ct, max_covar_name_blen, (pcp->misc_flags / kfMiscAffection01) & 1, 0, sample_include, &sample_ct);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
      }
      if (pcp->remove_if_expr.pheno_name) {
        reterr = KeepRemoveIf(&(pcp->remove_if_expr), pheno_cols, pheno_names, covar_cols, covar_names, raw_sample_ct, pheno_ct, max_pheno_name_blen, covar_ct, max_covar_name_blen, (pcp->misc_flags / kfMiscAffection01) & 1, 1, sample_include, &sample_ct);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
      }
      // meow
      if (pcp->keep_cats_fname || pcp->keep_cat_names_flattened) {
        reterr = KeepRemoveCats(pcp->keep_cats_fname, pcp->keep_cat_names_flattened, pcp->keep_cat_phenoname, pheno_cols, pheno_names, covar_cols, covar_names, raw_sample_ct, pheno_ct, max_pheno_name_blen, covar_ct, max_covar_name_blen, 0, pcp->max_thread_ct, sample_include, &sample_ct);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
      }
      if (pcp->remove_cats_fname || pcp->remove_cat_names_flattened) {
        reterr = KeepRemoveCats(pcp->remove_cats_fname, pcp->remove_cat_names_flattened, pcp->remove_cat_phenoname, pheno_cols, pheno_names, covar_cols, covar_names, raw_sample_ct, pheno_ct, max_pheno_name_blen, covar_ct, max_covar_name_blen, 1, pcp->max_thread_ct, sample_include, &sample_ct);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
      }
    }

    const uint32_t nonfounders = (pcp->misc_flags / kfMiscNonfounders) & 1;
    uint32_t founder_ct = 0;
    uint32_t male_ct = 0;
    uint32_t nosex_ct = 0;
    if (psamname[0]) {
      if (unlikely(!sample_ct)) {
        logerrputs("Error: No samples remaining after main filters.\n");
        goto Plink2Core_ret_DEGENERATE_DATA;
      }
      UpdateSampleSubsets(sample_include, raw_sample_ct, sample_ct, founder_info, &founder_ct, sex_nm, sex_male, &male_ct, &nosex_ct);
      if (pcp->filter_flags & kfFilterPsamReq) {
        const uint32_t female_ct = sample_ct - male_ct - nosex_ct;
        if (!nosex_ct) {
          logprintfww("%u sample%s (%u female%s, %u male%s; %u founder%s) remaining after main filters.\n", sample_ct, (sample_ct == 1)? "" : "s", female_ct, (female_ct == 1)? "" : "s", male_ct, (male_ct == 1)? "" : "s", founder_ct, (founder_ct == 1)? "" : "s");
        } else {
          logprintfww("%u sample%s (%u female%s, %u male%s, %u ambiguous; %u founder%s) remaining after main filters.\n", sample_ct, (sample_ct == 1)? "" : "s", female_ct, (female_ct == 1)? "" : "s", male_ct, (male_ct == 1)? "" : "s", nosex_ct, founder_ct, (founder_ct == 1)? "" : "s");
        }
        if (pheno_ct == 1) {
          const PhenoDtype pheno_type_code = pheno_cols[0].type_code;
          const uint32_t obs_ct = PopcountWordsIntersect(pheno_cols[0].nonmiss, sample_include, raw_sample_ctl);
          if (pheno_type_code == kPhenoDtypeCc) {
            const uint32_t case_ct = PopcountWordsIntersect(pheno_cols[0].data.cc, sample_include, raw_sample_ctl);
            const uint32_t ctrl_ct = obs_ct - case_ct;
            logprintf("%u case%s and %u control%s remaining after main filters.\n", case_ct, (case_ct == 1)? "" : "s", ctrl_ct, (ctrl_ct == 1)? "" : "s");
          } else {
            logprintf("%u %s phenotype value%s remaining after main filters.\n", obs_ct, (pheno_type_code == kPhenoDtypeQt)? "quantitative" : "categorical", (obs_ct == 1)? "" : "s");
          }
        }
      }
    }
    if (pcp->pheno_transform_flags & kfPhenoTransformSplitCat) {
      reterr = SplitCatPheno(pcp->split_cat_phenonames_flattened, sample_include, raw_sample_ct, pcp->pheno_transform_flags, &pheno_cols, &pheno_names, &pheno_ct, &max_pheno_name_blen, &covar_cols, &covar_names, &covar_ct, &max_covar_name_blen);
      if (unlikely(reterr)) {
        goto Plink2Core_ret_1;
      }
    }

    // quantnorm before variance-standardize, since at least that has a minor
    // effect, whereas the other order is pointless
    if (pcp->pheno_transform_flags & (kfPhenoTransformQuantnormPheno | kfPhenoTransformQuantnormAll)) {
      reterr = PhenoQuantileNormalize(pcp->quantnorm_flattened, sample_include, pheno_names, raw_sample_ct, sample_ct, pheno_ct, max_pheno_name_blen, 0, (pcp->pheno_transform_flags / kfPhenoTransformQuantnormPheno) & 1, pheno_cols);
      if (unlikely(reterr)) {
        goto Plink2Core_ret_1;
      }
    }
    if (pcp->pheno_transform_flags & (kfPhenoTransformQuantnormCovar | kfPhenoTransformQuantnormAll)) {
      reterr = PhenoQuantileNormalize((pcp->pheno_transform_flags & kfPhenoTransformQuantnormAll)? pcp->quantnorm_flattened : pcp->covar_quantnorm_flattened, sample_include, covar_names, raw_sample_ct, sample_ct, covar_ct, max_covar_name_blen, 1, (pcp->pheno_transform_flags / kfPhenoTransformQuantnormCovar) & 1, covar_cols);
      if (unlikely(reterr)) {
        goto Plink2Core_ret_1;
      }
    }

    if (pcp->pheno_transform_flags & (kfPhenoTransformVstdCovar | kfPhenoTransformVstdAll)) {
      const uint32_t is_covar_flag = (pcp->pheno_transform_flags / kfPhenoTransformVstdCovar) & 1;
      if (!is_covar_flag) {
        reterr = PhenoVarianceStandardize(pcp->vstd_flattened, sample_include, pheno_names, raw_sample_ct, pheno_ct, max_pheno_name_blen, 0, 0, pheno_cols);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
      }
      reterr = PhenoVarianceStandardize(pcp->vstd_flattened, sample_include, covar_names, raw_sample_ct, covar_ct, max_covar_name_blen, 1, is_covar_flag, covar_cols);
      if (unlikely(reterr)) {
        goto Plink2Core_ret_1;
      }
    }

    char* loop_cats_outname_endp1_backup = &(outname_end[1]);
    uintptr_t* loop_cats_sample_include_backup = nullptr;
    uintptr_t* loop_cats_founder_info_backup = nullptr;
    uintptr_t* loop_cats_sex_nm_backup = nullptr;
    uintptr_t* loop_cats_sex_male_backup = nullptr;
    uintptr_t* loop_cats_variant_include_backup = nullptr;
    uintptr_t* loop_cats_cat_include = nullptr;
    uint32_t loop_cats_sample_ct = 0;
    uint32_t loop_cats_variant_ct = variant_ct;
    uint32_t loop_cats_uidx = 0;
    uint32_t loop_cats_ct = 1;
    if (pcp->loop_cats_phenoname) {
      // 1. check phenotype names, verify it's a categorical pheno; fall back
      //    on checking covariates if no phenotype has same name
      // 2. remove phenotype/covariate, back up sample_include
      // 3. iterate through positive integer category numbers, skip empty
      //    categories
      const char* loop_cats_phenoname = pcp->loop_cats_phenoname;
      const uintptr_t name_blen = 1 + strlen(loop_cats_phenoname);
      if (name_blen <= max_pheno_name_blen) {
        // this boilerplate may belong in its own function
        for (uint32_t pheno_idx = 0; pheno_idx != pheno_ct; ++pheno_idx) {
          if (memequal(loop_cats_phenoname, &(pheno_names[pheno_idx * max_pheno_name_blen]), name_blen)) {
            PhenoCol* cur_pheno_col = &(pheno_cols[pheno_idx]);
            if (unlikely(cur_pheno_col->type_code != kPhenoDtypeCat)) {
              logerrprintfww("Error: '%s' is not a categorical phenotype.\n", loop_cats_phenoname);
              goto Plink2Core_ret_INCONSISTENT_INPUT;
            }
            loop_cats_pheno_col = cur_pheno_col;
            --pheno_ct;
            for (uint32_t pheno_idx2 = pheno_idx; pheno_idx2 != pheno_ct; ++pheno_idx2) {
              pheno_cols[pheno_idx2] = pheno_cols[pheno_idx2 + 1];
              memcpy(&(pheno_names[pheno_idx2 * max_pheno_name_blen]), &(pheno_names[(pheno_idx2 + 1) * max_pheno_name_blen]), max_pheno_name_blen);
            }
            break;
          }
        }
      }
      if ((!loop_cats_pheno_col) && (name_blen <= max_covar_name_blen)) {
        for (uint32_t covar_idx = 0; covar_idx != covar_ct; ++covar_idx) {
          if (memequal(loop_cats_phenoname, &(covar_names[covar_idx * max_covar_name_blen]), name_blen)) {
            PhenoCol* cur_covar_col = &(covar_cols[covar_idx]);
            if (unlikely(cur_covar_col->type_code != kPhenoDtypeCat)) {
              logerrprintfww("Error: '%s' is not a categorical covariate.\n", loop_cats_phenoname);
              goto Plink2Core_ret_INCONSISTENT_INPUT;
            }
            loop_cats_pheno_col = cur_covar_col;
            --covar_ct;
            for (uint32_t covar_idx2 = covar_idx; covar_idx2 != covar_ct; ++covar_idx2) {
              covar_cols[covar_idx2] = covar_cols[covar_idx2 + 1];
              memcpy(&(covar_names[covar_idx2 * max_covar_name_blen]), &(covar_names[(covar_idx2 + 1) * max_covar_name_blen]), max_covar_name_blen);
            }
            break;
          }
        }
      }
      if (unlikely(!loop_cats_pheno_col)) {
        logerrprintfww("Error: --loop-cats phenotype '%s' not loaded.\n", loop_cats_phenoname);
        goto Plink2Core_ret_INCONSISTENT_INPUT;
      }
      if (unlikely(
              bigstack_alloc_w(raw_sample_ctl, &loop_cats_sample_include_backup) ||
              bigstack_alloc_w(raw_sample_ctl, &loop_cats_founder_info_backup) ||
              bigstack_alloc_w(raw_sample_ctl, &loop_cats_sex_nm_backup) ||
              bigstack_alloc_w(raw_sample_ctl, &loop_cats_sex_male_backup) ||
              bigstack_alloc_w(1 + (loop_cats_pheno_col->nonnull_category_ct / kBitsPerWord), &loop_cats_cat_include))) {
        goto Plink2Core_ret_NOMEM;
      }
      if (variant_ct != raw_variant_ct) {
        if (unlikely(bigstack_alloc_w(raw_variant_ctl, &loop_cats_variant_include_backup))) {
          goto Plink2Core_ret_NOMEM;
        }
        memcpy(loop_cats_variant_include_backup, variant_include, raw_variant_ctl * sizeof(intptr_t));
      }
      BitvecAndCopy(sample_include, loop_cats_pheno_col->nonmiss, raw_sample_ctl, loop_cats_sample_include_backup);
      loop_cats_sample_ct = PopcountWords(loop_cats_sample_include_backup, raw_sample_ctl);
      loop_cats_ct = IdentifyRemainingCats(sample_include, loop_cats_pheno_col, sample_ct, loop_cats_cat_include);
      if (unlikely(!loop_cats_ct)) {
        logerrputs("Error: All --loop-cats categories are empty.\n");
        goto Plink2Core_ret_INCONSISTENT_INPUT;
      }
      logprintf("--loop-cats: %u categor%s present.\n", loop_cats_ct, (loop_cats_ct == 1)? "y" : "ies");
      BitvecAndCopy(sample_include, founder_info, raw_sample_ctl, loop_cats_founder_info_backup);
      BitvecAndCopy(sample_include, sex_nm, raw_sample_ctl, loop_cats_sex_nm_backup);
      BitvecAndCopy(sample_include, sex_male, raw_sample_ctl, loop_cats_sex_male_backup);
      *outname_end = '.';
    }
    const uintptr_t raw_allele_ct = allele_idx_offsets? allele_idx_offsets[raw_variant_ct] : (2 * raw_variant_ct);
    unsigned char* bigstack_mark_varfilter = g_bigstack_base;
    unsigned char* bigstack_end_mark_varfilter = g_bigstack_end;
    for (uint32_t loop_cats_idx = 0; loop_cats_idx != loop_cats_ct; ++loop_cats_idx) {
      BigstackDoubleReset(bigstack_mark_varfilter, bigstack_end_mark_varfilter);
      if (loop_cats_pheno_col) {
        loop_cats_uidx = AdvTo1Bit(loop_cats_cat_include, loop_cats_uidx + 1);
        const char* catname = loop_cats_pheno_col->category_names[loop_cats_uidx];
        const uint32_t catname_slen = strlen(catname);
        if (unlikely(catname_slen + S_CAST(uintptr_t, loop_cats_outname_endp1_backup - outname) > (kPglFnamesize - kMaxOutfnameExtBlen))) {
          logerrputs("Error: --loop-cats category name too long.\n");
          goto Plink2Core_ret_INCONSISTENT_INPUT;
        }
        outname_end = memcpya(loop_cats_outname_endp1_backup, catname, catname_slen);
        sample_ct = GetCatSamples(loop_cats_sample_include_backup, loop_cats_pheno_col, raw_sample_ctl, loop_cats_sample_ct, loop_cats_uidx, sample_include);
        memcpy(founder_info, loop_cats_founder_info_backup, raw_sample_ctl * sizeof(intptr_t));
        memcpy(sex_nm, loop_cats_sex_nm_backup, raw_sample_ctl * sizeof(intptr_t));
        memcpy(sex_male, loop_cats_sex_male_backup, raw_sample_ctl * sizeof(intptr_t));
        UpdateSampleSubsets(sample_include, raw_sample_ct, sample_ct, founder_info, &founder_ct, sex_nm, sex_male, &male_ct, &nosex_ct);
        variant_ct = loop_cats_variant_ct;
        if (loop_cats_variant_include_backup) {
          memcpy(variant_include, loop_cats_variant_include_backup, raw_variant_ctl * sizeof(intptr_t));
        } else {
          SetAllBits(variant_ct, variant_include);
        }
        logprintf("--loop-cats: Processing category '%s' (%u sample%s).\n", catname, sample_ct, (sample_ct == 1)? "" : "s");
      }

      uintptr_t* allele_presents = nullptr;

      // dosages are currently in 32768ths
      // same indexes as allele_storage.  We can't omit the last allele (in the
      // way allele_freqs does) because the sum isn't constant (missing
      // values).
      uint64_t* allele_dosages = nullptr;
      uint64_t* founder_allele_dosages = nullptr;

      AlleleCode* maj_alleles = nullptr;
      double* allele_freqs = nullptr;
      STD_ARRAY_PTR_DECL(uint32_t, 3, raw_geno_cts) = nullptr;
      STD_ARRAY_PTR_DECL(uint32_t, 3, founder_raw_geno_cts) = nullptr;
      unsigned char* bigstack_mark_allele_dosages = g_bigstack_base;
      const uint32_t keep_grm = GrmKeepIsNeeded(pcp->command_flags1, pcp->pca_flags);
      double* grm = nullptr;

      if (pcp->command_flags1 & kfCommand1WriteSamples) {
        snprintf(outname_end, kMaxOutfnameExtBlen, ".id");
        reterr = WriteSampleIds(sample_include, &pii.sii, outname, sample_ct);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
        logprintfww("--write-samples: Sample IDs written to %s .\n", outname);
        if (!(pcp->command_flags1 & (~(kfCommand1WriteSamples | kfCommand1Validate | kfCommand1PgenInfo | kfCommand1RmDupList)))) {
          continue;
        }
      }

      if (pgenname[0]) {
        const uint32_t decent_afreqs_needed = DecentAlleleFreqsAreNeeded(pcp->command_flags1, pcp->score_info.flags);
        const uint32_t maj_alleles_needed = MajAllelesAreNeeded(pcp->command_flags1, pcp->glm_info.flags);
        if (decent_afreqs_needed || maj_alleles_needed || IndecentAlleleFreqsAreNeeded(pcp->command_flags1, pcp->min_maf, pcp->max_maf)) {
          if (unlikely((!pcp->read_freq_fname) && ((sample_ct < 50) || ((!nonfounders) && (founder_ct < 50))) && decent_afreqs_needed && (!(pcp->misc_flags & kfMiscAllowBadFreqs)))) {
            if ((!nonfounders) && (sample_ct >= 50)) {
              logerrputs("Error: This run requires decent allele frequencies, but they aren't being\nloaded with --read-freq, and less than 50 founders are available to impute them\nfrom.  Possible solutions:\n* You can use --nonfounders to include nonfounders when imputing allele\n  frequencies.\n* You can generate (with --freq) or obtain an allele frequency file based on a\n  larger similar-population reference dataset, and load it with --read-freq.\n* (Not recommended) You can override this error with --bad-freqs.\n");
            } else {
              logerrputs("Error: This run requires decent allele frequencies, but they aren't being\nloaded with --read-freq, and less than 50 samples are available to impute them\nfrom.\nYou should generate (with --freq) or obtain an allele frequency file based on a\nlarger similar-population reference dataset, and load it with --read-freq.\n");
              if (nonfounders) {
                logerrputs("If you're certain you want to proceed without doing that, use --bad-freqs to\noverride this error.\n");
              } else if (sample_ct != founder_ct) {
                logerrputs("If you're certain you want to proceed without doing that, use --bad-freqs to\noverride this error, and consider using --nonfounders as well.\n");
              }
            }
            goto Plink2Core_ret_DEGENERATE_DATA;
          }
          if (maj_alleles_needed) {
            if (unlikely(bigstack_alloc_ac(raw_variant_ct, &maj_alleles))) {
              goto Plink2Core_ret_NOMEM;
            }
          }
          //   allele_freqs[allele_idx_offsets[variant_uidx] - variant_uidx]
          // stores the frequency estimate for the reference allele; if there's
          // more than 1 alt allele, next element stores alt1 freq, etc.  To
          // save memory, we omit the last alt.
          if (unlikely(bigstack_alloc_d(raw_allele_ct - raw_variant_ct, &allele_freqs))) {
            goto Plink2Core_ret_NOMEM;
          }
        }
        uint32_t x_start = 0;
        uint32_t x_len = 0;
        uint32_t hwe_x_probs_needed = 0;
        uint32_t x_code;
        if (XymtExists(cip, kChrOffsetX, &x_code) && (!(vpos_sortstatus & kfUnsortedVarSplitChr))) {
          const uint32_t x_chr_fo_idx = cip->chr_idx_to_foidx[x_code];
          x_start = cip->chr_fo_vidx_start[x_chr_fo_idx];
          const uint32_t x_end = cip->chr_fo_vidx_start[x_chr_fo_idx + 1];
          x_len = x_end - x_start;
          if (x_len && ((pcp->command_flags1 & kfCommand1Hardy) || (pcp->hwe_thresh != 0.0)) && (!AllBitsAreZero(variant_include, x_start, x_end))) {
            if (nonfounders) {
              hwe_x_probs_needed = (sample_ct > nosex_ct);
            } else {
              // at least one founder with known gender?
              hwe_x_probs_needed = !IntersectionIsEmpty(founder_info, sex_nm, raw_sample_ctl);
            }
          }
        }
        const uint32_t first_hap_uidx = GetFirstHaploidUidx(cip, vpos_sortstatus);
        if (make_plink2_flags & kfMakePlink2TrimAlts) {
          if (unlikely(bigstack_alloc_w(BitCtToWordCt(raw_allele_ct), &allele_presents))) {
            goto Plink2Core_ret_NOMEM;
          }
        }
        bigstack_mark_allele_dosages = g_bigstack_base;
        uint32_t regular_freqcounts_needed = (allele_presents != nullptr);
        if (AlleleDosagesAreNeeded(pcp->misc_flags, (allele_freqs != nullptr), pcp->min_allele_dosage, pcp->max_allele_dosage, &regular_freqcounts_needed)) {
          if (unlikely(bigstack_alloc_u64(raw_allele_ct, &allele_dosages))) {
            goto Plink2Core_ret_NOMEM;
          }
        }
        if (FounderAlleleDosagesAreNeeded(pcp->misc_flags, (allele_freqs != nullptr), pcp->min_allele_dosage, pcp->max_allele_dosage, &regular_freqcounts_needed)) {
          if ((founder_ct == sample_ct) && allele_dosages) {
            founder_allele_dosages = allele_dosages;
          } else {
            if (unlikely(bigstack_alloc_u64(raw_allele_ct, &founder_allele_dosages))) {
              goto Plink2Core_ret_NOMEM;
            }
          }
        }
        double* imp_r2_vals = nullptr;
        const uint32_t is_minimac3_r2 = (pcp->freq_rpt_flags & kfAlleleFreqColMinimac3R2) || (pcp->minimac3_r2_max != 0.0);
        if (is_minimac3_r2 || (pcp->freq_rpt_flags & kfAlleleFreqColMachR2) || (pcp->mach_r2_max != 0.0)) {
          if (unlikely(bigstack_alloc_d(raw_variant_ct, &imp_r2_vals))) {
            goto Plink2Core_ret_NOMEM;
          }
        }

        unsigned char* bigstack_mark_geno_cts = g_bigstack_base;

        // no longer includes hethaps by default
        uint32_t* variant_missing_hc_cts = nullptr;
        uint32_t* variant_hethap_cts = nullptr;
        if (VariantMissingHcCtsAreNeeded(pcp->command_flags1, pcp->misc_flags, pcp->geno_thresh, pcp->missing_rpt_flags)) {
          if (unlikely(bigstack_alloc_u32(raw_variant_ct, &variant_missing_hc_cts))) {
            goto Plink2Core_ret_NOMEM;
          }
          if (VariantHethapCtsAreNeeded(pcp->command_flags1, pcp->misc_flags, pcp->geno_thresh, pcp->missing_rpt_flags, first_hap_uidx)) {
            // first_hap_uidx offset can save an entire GB...
            if (unlikely(bigstack_alloc_u32(raw_variant_ct - first_hap_uidx, &variant_hethap_cts))) {
              goto Plink2Core_ret_NOMEM;
            }
          }
        }
        uint32_t* variant_missing_dosage_cts = nullptr;
        if (VariantMissingDosageCtsAreNeeded(pcp->command_flags1, pcp->misc_flags, pcp->geno_thresh, pcp->missing_rpt_flags)) {
          if ((!variant_missing_hc_cts) || (pgfi.gflags & kfPgenGlobalDosagePresent)) {
            if (unlikely(bigstack_alloc_u32(raw_variant_ct, &variant_missing_dosage_cts))) {
              goto Plink2Core_ret_NOMEM;
            }
          } else {
            variant_missing_dosage_cts = variant_missing_hc_cts;
          }
        }
        STD_ARRAY_PTR_DECL(uint32_t, 3, x_male_geno_cts) = nullptr;
        STD_ARRAY_PTR_DECL(uint32_t, 3, founder_x_male_geno_cts) = nullptr;
        STD_ARRAY_PTR_DECL(uint32_t, 3, x_nosex_geno_cts) = nullptr;
        STD_ARRAY_PTR_DECL(uint32_t, 3, founder_x_nosex_geno_cts) = nullptr;
        // [0] = homref ct, [1] = het ref-altx total, [2] = nonref diploid
        //   total
        // use unfiltered indexes, since we remove more variants later
        if (RawGenoCtsAreNeeded(pcp->command_flags1, pcp->misc_flags, pcp->hwe_thresh)) {
          if (unlikely(BIGSTACK_ALLOC_STD_ARRAY(uint32_t, 3, raw_variant_ct, &raw_geno_cts))) {
            goto Plink2Core_ret_NOMEM;
          }
          if (x_len) {
            if (male_ct) {
              if (unlikely(BIGSTACK_ALLOC_STD_ARRAY(uint32_t, 3, x_len, &x_male_geno_cts))) {
                goto Plink2Core_ret_NOMEM;
              }
            }
            if (nosex_ct && hwe_x_probs_needed && nonfounders) {
              if (unlikely(BIGSTACK_ALLOC_STD_ARRAY(uint32_t, 3, x_len, &x_nosex_geno_cts))) {
                goto Plink2Core_ret_NOMEM;
              }
            }
          }
        }
        if (FounderRawGenoCtsAreNeeded(pcp->command_flags1, pcp->misc_flags, pcp->hwe_thresh)) {
          if ((founder_ct == sample_ct) && raw_geno_cts) {
            founder_raw_geno_cts = raw_geno_cts;
            founder_x_male_geno_cts = x_male_geno_cts;
          } else {
            if (unlikely(BIGSTACK_ALLOC_STD_ARRAY(uint32_t, 3, raw_variant_ct, &founder_raw_geno_cts))) {
              goto Plink2Core_ret_NOMEM;
            }
            if (x_len && male_ct) {
              if (!IntersectionIsEmpty(founder_info, sex_male, raw_sample_ctl)) {
                if (unlikely(BIGSTACK_ALLOC_STD_ARRAY(uint32_t, 3, x_len, &founder_x_male_geno_cts))) {
                  goto Plink2Core_ret_NOMEM;
                }
              }
            }
          }
          if (nosex_ct && hwe_x_probs_needed && (!nonfounders)) {
            const uint32_t founder_knownsex_ct = PopcountWordsIntersect(founder_info, sex_nm, raw_sample_ctl);
            if (founder_knownsex_ct < founder_ct) {
              if ((founder_ct == sample_ct) && x_nosex_geno_cts) {
                // shouldn't be possible for now, since x_nosex_geno_cts can
                // only be non-null when nonfounders is true
                assert(0);
                // founder_x_nosex_geno_cts = x_nosex_geno_cts;
              } else {
                if (unlikely(BIGSTACK_ALLOC_STD_ARRAY(uint32_t, 3, x_len, &founder_x_nosex_geno_cts))) {
                  goto Plink2Core_ret_NOMEM;
                }
              }
            }
          }
        }
        unsigned char* bigstack_mark_read_freqs = g_bigstack_base;
        regular_freqcounts_needed = regular_freqcounts_needed || variant_missing_hc_cts || variant_missing_dosage_cts || variant_hethap_cts || raw_geno_cts || founder_raw_geno_cts || imp_r2_vals;
        uintptr_t* variant_afreqcalc = nullptr;
        uint32_t afreqcalc_variant_ct = 0;
        if (allele_freqs) {
          if (pcp->read_freq_fname) {
            reterr = ReadAlleleFreqs(variant_include, variant_ids, allele_idx_offsets, allele_storage, pcp->read_freq_fname, raw_variant_ct, variant_ct, max_allele_ct, max_variant_id_slen, max_allele_slen, (pcp->misc_flags / kfMiscMafSucc) & 1, pcp->max_thread_ct, allele_freqs, &variant_afreqcalc);
            if (unlikely(reterr)) {
              goto Plink2Core_ret_1;
            }
            if (variant_ct) {
              afreqcalc_variant_ct = PopcountWords(variant_afreqcalc, raw_variant_ctl);
            }
          } else {
            variant_afreqcalc = variant_include;
            afreqcalc_variant_ct = variant_ct;
          }
        } else if (pcp->read_freq_fname) {
          logerrprintf("Warning: Ignoring --read-freq since no command would use the frequencies.\n");
        }
        if (regular_freqcounts_needed || afreqcalc_variant_ct) {
          // note that --geno depends on different handling of X/Y than --maf.

          // possible todo: "free" these arrays early in some cases
          // todo: oblig-missing

          // bugfix (22 Nov 2017): given "--genotyping-rate dosage" on a file
          // with no dosage data, variant_missing_hc_cts may be nullptr while
          // variant_missing_dosage_cts is zero-initialized.  In this case,
          // it's fine to pass variant_missing_dosage_cts in the
          // hardcall-missing-count slot... and it's NOT fine to pass in
          // nullptrs for both missing-count arrays...
          const uint32_t dosageless_file = !(pgfi.gflags & kfPgenGlobalDosagePresent);
          reterr = LoadAlleleAndGenoCounts(sample_include, founder_info, sex_nm, sex_male, regular_freqcounts_needed? variant_include : variant_afreqcalc, cip, allele_idx_offsets, raw_sample_ct, sample_ct, founder_ct, male_ct, nosex_ct, raw_variant_ct, regular_freqcounts_needed? variant_ct : afreqcalc_variant_ct, first_hap_uidx, is_minimac3_r2, pcp->max_thread_ct, pgr_alloc_cacheline_ct, &pgfi, allele_presents, allele_dosages, founder_allele_dosages, ((!variant_missing_hc_cts) && dosageless_file)? variant_missing_dosage_cts : variant_missing_hc_cts, dosageless_file? nullptr : variant_missing_dosage_cts, variant_hethap_cts, raw_geno_cts, founder_raw_geno_cts, x_male_geno_cts, founder_x_male_geno_cts, x_nosex_geno_cts, founder_x_nosex_geno_cts, imp_r2_vals);
          if (unlikely(reterr)) {
            goto Plink2Core_ret_1;
          }
          if (pcp->command_flags1 & kfCommand1GenotypingRate) {
            // possible todo: also report this opportunistically
            // (variant_missing_hc_cts filled for other reasons).  worth
            // multithreading in that case.
            const uint32_t is_dosage = (pcp->misc_flags / kfMiscGenotypingRateDosage) & 1;
            ReportGenotypingRate(variant_include, cip, is_dosage? variant_missing_dosage_cts : variant_missing_hc_cts, raw_sample_ct, sample_ct, male_ct, variant_ct, is_dosage);
            if (!(pcp->command_flags1 & (~(kfCommand1GenotypingRate | kfCommand1WriteSamples | kfCommand1Validate | kfCommand1PgenInfo | kfCommand1RmDupList)))) {
              continue;
            }
          }
        }
        if (allele_freqs) {
          if (afreqcalc_variant_ct) {
            const uint32_t maf_succ = (pcp->misc_flags / kfMiscMafSucc) & 1;
            ComputeAlleleFreqs(variant_afreqcalc, allele_idx_offsets, nonfounders? allele_dosages : founder_allele_dosages, afreqcalc_variant_ct, maf_succ, allele_freqs);
          }
          if (maj_alleles) {
            ComputeMajAlleles(variant_include, allele_idx_offsets, allele_freqs, variant_ct, maj_alleles);
          }
        }
        BigstackReset(bigstack_mark_read_freqs);

        if (pcp->command_flags1 & kfCommand1AlleleFreq) {
          reterr = WriteAlleleFreqs(variant_include, cip, variant_bps, variant_ids, allele_idx_offsets, allele_storage, nonfounders? allele_dosages : founder_allele_dosages, imp_r2_vals, pcp->freq_ref_binstr, pcp->freq_alt1_binstr, variant_ct, max_allele_ct, max_allele_slen, pcp->freq_rpt_flags, pcp->max_thread_ct, nonfounders, outname, outname_end);
          if (unlikely(reterr)) {
            goto Plink2Core_ret_1;
          }
          if (!(pcp->command_flags1 & (~(kfCommand1GenotypingRate | kfCommand1AlleleFreq | kfCommand1WriteSamples | kfCommand1Validate | kfCommand1PgenInfo | kfCommand1RmDupList)))) {
            continue;
          }
        }
        if (pcp->command_flags1 & kfCommand1GenoCounts) {
          reterr = WriteGenoCounts(sample_include, sex_male, variant_include, cip, variant_bps, variant_ids, allele_idx_offsets, allele_storage, raw_geno_cts, x_male_geno_cts, raw_sample_ct, sample_ct, male_ct, variant_ct, x_start, max_allele_slen, pcp->geno_counts_flags, pcp->max_thread_ct, &simple_pgr, outname, outname_end);
          if (unlikely(reterr)) {
            goto Plink2Core_ret_1;
          }
          if (!(pcp->command_flags1 & (~(kfCommand1GenotypingRate | kfCommand1AlleleFreq | kfCommand1GenoCounts | kfCommand1WriteSamples | kfCommand1Validate | kfCommand1PgenInfo | kfCommand1RmDupList)))) {
            continue;
          }
        }

        if (pcp->command_flags1 & kfCommand1MissingReport) {
          reterr = WriteMissingnessReports(sample_include, &pii.sii, sex_male, pheno_cols, pheno_names, sample_missing_hc_cts, sample_missing_dosage_cts, sample_hethap_cts, variant_include, cip, variant_bps, variant_ids, allele_idx_offsets, allele_storage, variant_missing_hc_cts, variant_missing_dosage_cts, variant_hethap_cts, sample_ct, male_ct, pheno_ct, max_pheno_name_blen, variant_ct, max_allele_slen, variant_hethap_cts? first_hap_uidx : 0x7fffffff, pcp->missing_rpt_flags, pcp->max_thread_ct, outname, outname_end);
          if (unlikely(reterr)) {
            goto Plink2Core_ret_1;
          }
          if (!(pcp->command_flags1 & (~(kfCommand1GenotypingRate | kfCommand1AlleleFreq | kfCommand1GenoCounts | kfCommand1MissingReport | kfCommand1WriteSamples | kfCommand1Validate | kfCommand1PgenInfo | kfCommand1RmDupList)))) {
            continue;
          }
        }

        if (pcp->geno_thresh != 1.0) {
          const uint32_t geno_hh_missing = S_CAST(uint32_t, pcp->misc_flags & kfMiscGenoHhMissing);
          EnforceGenoThresh(cip, (pcp->misc_flags & kfMiscGenoDosage)? variant_missing_dosage_cts : variant_missing_hc_cts, geno_hh_missing? variant_hethap_cts : nullptr, sample_ct, male_ct, geno_hh_missing? first_hap_uidx : 0x7fffffff, pcp->geno_thresh, variant_include, &variant_ct);
        }

        if ((pcp->command_flags1 & kfCommand1Hardy) || (pcp->hwe_thresh != 0.0)) {
          if (cip->haploid_mask[0] & 1) {
            if (unlikely(pcp->command_flags1 & kfCommand1Hardy)) {
              logerrputs("Error: --hardy is pointless on an all-haploid genome.\n");
              goto Plink2Core_ret_INCONSISTENT_INPUT;
            }
            // could make hwe_thresh non-const and set it to 0.0 earlier on in
            // this case
            logerrputs("Warning: --hwe has no effect since entire genome is haploid.\n");
          } else {
            STD_ARRAY_PTR_DECL(uint32_t, 3, hwe_geno_cts) = nonfounders? raw_geno_cts : founder_raw_geno_cts;
            STD_ARRAY_PTR_DECL(uint32_t, 3, hwe_x_male_geno_cts) = nonfounders? x_male_geno_cts : founder_x_male_geno_cts;
            STD_ARRAY_PTR_DECL(uint32_t, 3, hwe_x_nosex_geno_cts) = nonfounders? x_nosex_geno_cts : founder_x_nosex_geno_cts;

            // For multiallelic variants, perform one 'biallelic' test per
            // allele.  This is much, much faster than e.g. triallelic exact
            // tests, and the resulting set of p-values are arguably more
            // useful anyway.

            // One entry per autosomal multiallelic alt allele.
            // Only two cells per entry, since third can be inferred by
            // subtracting from the nonmissing genotype count (this is
            // potentially a memory hog).
            STD_ARRAY_PTR_DECL(uint32_t, 2, autosomal_xgeno_cts) = nullptr;
            // one entry per chrX multiallelic alt allele.
            STD_ARRAY_PTR_DECL(uint32_t, 2, x_knownsex_xgeno_cts) = nullptr;
            STD_ARRAY_PTR_DECL(uint32_t, 2, x_male_xgeno_cts) = nullptr;
            uint32_t hwe_x_ct = 0;
            if (hwe_x_probs_needed) {
              hwe_x_ct = CountChrVariantsUnsafe(variant_include, cip, cip->xymt_codes[kChrOffsetX]);
              // hwe_x_ct == 0 possible when hwe_x_probs_needed set, if --geno
              // filters out all chrX variants
            }
            uintptr_t autosomal_xallele_ct = 0;
            uintptr_t x_xallele_ct = 0;
            if (allele_idx_offsets) {
              const uint32_t autosomal_variant_ct = variant_ct - hwe_x_ct - CountNonAutosomalVariants(variant_include, cip, 0, 1);
              const uint32_t chr_ct = cip->chr_ct;
              for (uint32_t chr_fo_idx = 0; chr_fo_idx != chr_ct; ++chr_fo_idx) {
                const uint32_t chr_idx = cip->chr_file_order[chr_fo_idx];
                if ((chr_idx != x_code) && (!IsSet(cip->haploid_mask, chr_idx))) {
                  autosomal_xallele_ct += CountExtraAlleles(variant_include, allele_idx_offsets, cip->chr_fo_vidx_start[chr_fo_idx], cip->chr_fo_vidx_start[chr_fo_idx + 1], 1);
                }
              }
              if (hwe_x_ct) {
                x_xallele_ct = CountExtraAlleles(variant_include, allele_idx_offsets, x_start, x_start + x_len, 1);
              }
              if (autosomal_xallele_ct || x_xallele_ct) {
                if (autosomal_xallele_ct) {
                  if (unlikely(BIGSTACK_ALLOC_STD_ARRAY(uint32_t, 2, autosomal_xallele_ct, &autosomal_xgeno_cts))) {
                    goto Plink2Core_ret_NOMEM;
                  }
                }
                if (x_xallele_ct) {
                  if (unlikely(
                          BIGSTACK_ALLOC_STD_ARRAY(uint32_t, 2, x_xallele_ct, &x_knownsex_xgeno_cts))) {
                    goto Plink2Core_ret_NOMEM;
                  }
                  if (hwe_x_male_geno_cts) {
                    if (unlikely(
                            BIGSTACK_ALLOC_STD_ARRAY(uint32_t, 2, x_xallele_ct, &x_male_xgeno_cts))) {
                      goto Plink2Core_ret_NOMEM;
                    }
                  }
                }
                reterr = GetMultiallelicMarginalCounts(nonfounders? sample_include : founder_info, sex_nm, sex_male, variant_include, cip, allele_idx_offsets, hwe_geno_cts, raw_sample_ct, autosomal_variant_ct, autosomal_xallele_ct, hwe_x_ct, x_xallele_ct, &simple_pgr, x_male_xgeno_cts, autosomal_xgeno_cts, x_knownsex_xgeno_cts);
                if (unlikely(reterr)) {
                  goto Plink2Core_ret_1;
                }
              }
            }

            double* hwe_x_pvals = nullptr;
            if (hwe_x_ct && ((pcp->hwe_thresh != 0.0) || (pcp->hardy_flags & kfHardyColP))) {
              // support suppression of --hardy p column (with a gigantic
              // dataset, maybe it's reasonable to stick to femalep, etc.)
              uint32_t hwe_midp;
              if (pcp->command_flags1 & kfCommand1Hardy) {
                hwe_midp = (pcp->hardy_flags / kfHardyMidp) & 1;
                if (pcp->hwe_thresh != 0.0) {
                  const uint32_t hwe_midp2 = (pcp->misc_flags / kfMiscHweMidp) & 1;
                  if (unlikely(hwe_midp != hwe_midp2)) {
                    // could support this efficiently, but why bother...
                    logerrputs("Error: --hardy and --hwe must have identical midp settings when chrX is\npresent.\n");
                    goto Plink2Core_ret_INVALID_CMDLINE;
                  }
                }
              } else {
                hwe_midp = (pcp->misc_flags / kfMiscHweMidp) & 1;
              }
              reterr = ComputeHweXPvals(variant_include, allele_idx_offsets, hwe_geno_cts, hwe_x_male_geno_cts, hwe_x_nosex_geno_cts, x_knownsex_xgeno_cts, x_male_xgeno_cts, x_start, hwe_x_ct, x_xallele_ct, hwe_midp, pcp->max_thread_ct, &hwe_x_pvals);
              if (unlikely(reterr)) {
                goto Plink2Core_ret_1;
              }
            }
            if (pcp->command_flags1 & kfCommand1Hardy) {
              reterr = HardyReport(variant_include, cip, variant_bps, variant_ids, allele_idx_offsets, allele_storage, hwe_geno_cts, autosomal_xgeno_cts, hwe_x_male_geno_cts, hwe_x_nosex_geno_cts, x_knownsex_xgeno_cts, x_male_xgeno_cts, hwe_x_pvals, variant_ct, hwe_x_ct, max_allele_slen, pcp->output_min_ln, pcp->hardy_flags, pcp->max_thread_ct, nonfounders, outname, outname_end);
              if (unlikely(reterr)) {
                goto Plink2Core_ret_1;
              }
              if (!(pcp->command_flags1 & (~(kfCommand1GenotypingRate | kfCommand1AlleleFreq | kfCommand1GenoCounts | kfCommand1MissingReport | kfCommand1Hardy | kfCommand1WriteSamples | kfCommand1Validate | kfCommand1PgenInfo | kfCommand1RmDupList)))) {
                continue;
              }
            }
            if (pcp->hwe_thresh != 0.0) {
              // assumes no filtering between hwe_x_pvals[] computation and
              // here
              EnforceHweThresh(cip, allele_idx_offsets, hwe_geno_cts, autosomal_xgeno_cts, hwe_x_male_geno_cts, hwe_x_nosex_geno_cts, x_knownsex_xgeno_cts, x_male_xgeno_cts, hwe_x_pvals, pcp->misc_flags, pcp->hwe_thresh, nonfounders, variant_include, &variant_ct);
            }
          }
        }
        // raw_geno_cts/founder_raw_geno_cts/hwe_x_pvals no longer needed
        BigstackReset(bigstack_mark_geno_cts);

        if ((pcp->min_maf != 0.0) || (pcp->max_maf != 1.0) || pcp->min_allele_dosage || (pcp->max_allele_dosage != (~0LLU))) {
          EnforceFreqConstraints(allele_idx_offsets, nonfounders? allele_dosages : founder_allele_dosages, allele_freqs, pcp->filter_modes, pcp->min_maf, pcp->max_maf, pcp->min_allele_dosage, pcp->max_allele_dosage, variant_include, &variant_ct);
        }

        if (imp_r2_vals) {
          if (pcp->mach_r2_max != 0.0) {
            EnforceImpR2Thresh(cip, imp_r2_vals, pcp->mach_r2_min, pcp->mach_r2_max, 0, variant_include, &variant_ct);
          } else if (pcp->minimac3_r2_max != 0.0) {
            EnforceImpR2Thresh(cip, imp_r2_vals, pcp->minimac3_r2_min, pcp->minimac3_r2_max, 1, variant_include, &variant_ct);
          }
          BigstackReset(imp_r2_vals);
        }
      }

      if (pcp->min_bp_space) {
        if (vpos_sortstatus & kfUnsortedVarBp) {
          logerrputs("Error: --bp-space requires a sorted .pvar/.bim.  Retry this command after using\n--make-pgen/--make-bed + --sort-vars to sort your data.\n");
        }
        EnforceMinBpSpace(cip, variant_bps, pcp->min_bp_space, variant_include, &variant_ct);
      }

      if (pcp->filter_flags & kfFilterPvarReq) {
        if (unlikely(!variant_ct)) {
          // do we want this to be conditionally acceptable?
          logerrputs("Error: No variants remaining after main filters.\n");
          goto Plink2Core_ret_DEGENERATE_DATA;
        }
        logprintf("%u variant%s remaining after main filters.\n", variant_ct, (variant_ct == 1)? "" : "s");
      }

      if (pcp->command_flags1 & kfCommand1SampleCounts) {
        reterr = SampleCounts(sample_include, &pii.sii, sex_nm, sex_male, variant_include, cip, allele_idx_offsets, allele_storage, raw_sample_ct, sample_ct, male_ct, raw_variant_ct, variant_ct, max_allele_ct, pcp->sample_counts_flags, pcp->max_thread_ct, pgr_alloc_cacheline_ct, &pgfi, outname, outname_end);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
      }

      if (pcp->command_flags1 & kfCommand1Sdiff) {
        reterr = Sdiff(sample_include, &pii.sii, sex_nm, sex_male, variant_include, cip, variant_bps, variant_ids, allele_idx_offsets, allele_storage, &(pcp->sdiff_info), raw_sample_ct, sample_ct, variant_ct, (pcp->misc_flags / kfMiscIidSid) & 1, pcp->max_thread_ct, &simple_pgr, outname, outname_end);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
      }

      if (pcp->command_flags1 & (kfCommand1MakeKing | kfCommand1KingCutoff)) {
        uintptr_t* prev_sample_include = nullptr;
        const uint32_t prev_sample_ct = sample_ct;
        if (pcp->king_cutoff != -1) {
          if (unlikely(bigstack_alloc_w(raw_sample_ctl, &prev_sample_include))) {
            goto Plink2Core_ret_NOMEM;
          }
          memcpy(prev_sample_include, sample_include, raw_sample_ctl * sizeof(intptr_t));
        }
        uint32_t rel_check = 0;
        if (pcp->king_flags & kfKingRelCheck) {
          if (OnlyOneFid(sample_include, &pii.sii, sample_ct)) {
            logerrputs("Warning: --make-king-table 'rel-check' modifier has no effect since only one\nFID is present.\n");
          } else {
            rel_check = 1;
          }
        }
        if (pcp->king_table_subset_fname || rel_check) {
          // command-line parser currently guarantees --king-table-subset and
          // "--make-king-table rel-check" aren't used with --king-cutoff or
          // --make-king
          // probable todo: --king-cutoff-table which can use .kin0 as input
          reterr = CalcKingTableSubset(sample_include, &pii.sii, variant_include, cip, pcp->king_table_subset_fname, raw_sample_ct, sample_ct, raw_variant_ct, variant_ct, pcp->king_table_filter, pcp->king_table_subset_thresh, rel_check, pcp->king_flags, pcp->parallel_idx, pcp->parallel_tot, pcp->max_thread_ct, &simple_pgr, outname, outname_end);
          if (unlikely(reterr)) {
            goto Plink2Core_ret_1;
          }
        } else {
          if (king_cutoff_fprefix) {
            reterr = KingCutoffBatch(&pii.sii, raw_sample_ct, pcp->king_cutoff, sample_include, king_cutoff_fprefix, &sample_ct);
          } else {
            reterr = CalcKing(&pii.sii, variant_include, cip, raw_sample_ct, raw_variant_ct, variant_ct, pcp->king_cutoff, pcp->king_table_filter, pcp->king_flags, pcp->parallel_idx, pcp->parallel_tot, pcp->max_thread_ct, pgr_alloc_cacheline_ct, &pgfi, &simple_pgr, sample_include, &sample_ct, outname, outname_end);
          }
          if (unlikely(reterr)) {
            goto Plink2Core_ret_1;
          }
          if (pcp->king_cutoff != -1) {
            snprintf(outname_end, kMaxOutfnameExtBlen, ".king.cutoff.in.id");
            reterr = WriteSampleIds(sample_include, &pii.sii, outname, sample_ct);
            if (unlikely(reterr)) {
              goto Plink2Core_ret_1;
            }
            snprintf(&(outname_end[13]), kMaxOutfnameExtBlen - 13, "out.id");
            BitvecInvmask(sample_include, raw_sample_ctl, prev_sample_include);
            const uint32_t removed_sample_ct = prev_sample_ct - sample_ct;
            reterr = WriteSampleIds(prev_sample_include, &pii.sii, outname, removed_sample_ct);
            if (unlikely(reterr)) {
              goto Plink2Core_ret_1;
            }
            BigstackReset(prev_sample_include);
            outname_end[13] = '\0';
            logprintfww("--king-cutoff: Excluded sample ID%s written to %sout.id , and %u remaining sample ID%s written to %sin.id .\n", (removed_sample_ct == 1)? "" : "s", outname, sample_ct, (sample_ct == 1)? "" : "s", outname);
            UpdateSampleSubsets(sample_include, raw_sample_ct, sample_ct, founder_info, &founder_ct, sex_nm, sex_male, &male_ct, &nosex_ct);
          }
        }
      }
      if ((pcp->command_flags1 & kfCommand1MakeRel) || keep_grm) {
        reterr = CalcGrm(sample_include, &pii.sii, variant_include, cip, allele_idx_offsets, maj_alleles, allele_freqs, raw_sample_ct, sample_ct, raw_variant_ct, variant_ct, pcp->grm_flags, pcp->parallel_idx, pcp->parallel_tot, pcp->max_thread_ct, &simple_pgr, outname, outname_end, keep_grm? (&grm) : nullptr);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
        // Retire --rel-cutoff, since --king-cutoff is pretty clearly better.
        // KING-robust has significant systematic biases when interracial
        // couples are involved, though.  Still may be okay for first-degree
        // in that context, but something like PC-Relate should be added soon.
        //
        // In addition to even better kinship coefficients, PC-Relate also
        // provides a replacement for --genome's obsolete IBD sharing
        // estimates, and improved inbreeding coefficients.  There may still be
        // work to do on handling of highly-inbred populations, but PC-Relate
        // appears to be enough of an advance to warrant including now without
        // waiting for further refinements.
        //
        // Assuming we follow through with PC-Relate:
        //   1. use basic PC-AiR method to estimate top PCs in a way that's
        //      more resistant to related samples:
        //      a. compute KING-robust matrix.  keep in memory for now; maybe
        //         allow this to spill to temporary file later since this is
        //         a major scaling limitation.  probably store as floats,
        //         because of scaling; even then, 100k samples requires over
        //         280 GB, and it goes up quadratically from there.
        //         (though the problem isn't actually very serious, since your
        //         PCs will practically always be good enough if you select a
        //         size-10k random sample)
        //      b. also compute ancestry divergence matrix, with entries
        //           0.5 * (1 - \frac{\sum (g_1 - g_2)^2}{hets_1 + hets_2})
        //         main CalcKing() loop doesn't need to be changed; numerator
        //         of fraction is 4 * ibs0_ct + het1hom2_ct + het2hom1_ct,
        //         denominator is 2 * hethet_ct + het1hom2_ct + het2hom1_ct
        //      c. use the algorithm in Appendix B of
        //           https://www.ncbi.nlm.nih.gov/pmc/articles/PMC4836868/
        //         to select an unrelated subset.  (unfortunately, step 7 is
        //         random, so we can't always test for perfect concordance with
        //         the R package.)  This minimizes the risk of getting PCs
        //         which map to e.g. a sample-duplicate cluster instead of
        //         broad ancestry.
        //      d. compute PCs on the unrelated subset, project to related
        //         subset in the usual way.  (since overall algorithm has a
        //         random component anyway, there's no longer much of a point
        //         to defaulting to nonrandom PCA for >5k samples; instead,
        //         have 'random-pca'/'nonrandom-pca' modifiers to force, and
        //         default to switching over at 5k)
        //         allow these PCs to be saved to disk, and later steps to
        //         load them as input.
        //         extra MAF cutoff should be applied here.
        //         may want default PC count to be 6-8 rather than 10.
        //   2. for kinship matrix:
        //      a. for each variant,
        //         i. estimate individual-specific allele frequencies, by
        //            performing linear regression with y = genotype vector and
        //            X = constant column + top PCs from step 1; clip to
        //            [epsilon, 1 - epsilon]
        //            (this is also a step in the computations to follow, but
        //            we don't default to saving the intermediate result to
        //            disk because it's huge, and quick to compute from PCs +
        //            genotype matrix)
        //         ii. compute vector of (g-2q)/sqrt(q(1-q)) values
        //      b. same incremental matrix multiplies as GRM computation,
        //         dsyrk() is our friend
        //   3. for inbreeding coefficients, see equation 6 of
        //        https://www.ncbi.nlm.nih.gov/pubmed/26748516
        //   4. for IBD sharing, see equations 9 and 10 (note that inbreeding
        //      coefficient enters in to 9, and kinship enters into 10).
        //      when classifying first-degree relationships (probably want to
        //      add an option to do this; can consider second-degree too if it
        //      passes accuracy tests), parent-offspring corresponds to
        //      P(IBD=0) < 2^{-9/2} (todo: check theoretical justification for
        //      this threshold, if it's arbitrary it should be tunable)
        //   5. we may want to extend other allele-frequency-dependent
        //      commonly-used functions (--check-sex/--impute-sex is the most
        //      important one that comes to mind) to be able to use
        //      individual-specific allele frequencies.

        // possible todo: unrelated heritability?
      }
#ifndef NOLAPACK
      if (pcp->command_flags1 & kfCommand1Pca) {
        // if the GRM is on the stack, this always frees it
        reterr = CalcPca(sample_include, &pii.sii, variant_include, cip, variant_bps, variant_ids, allele_idx_offsets, allele_storage, maj_alleles, allele_freqs, raw_sample_ct, sample_ct, raw_variant_ct, variant_ct, max_allele_slen, pcp->pca_ct, pcp->pca_flags, pcp->max_thread_ct, &simple_pgr, sfmtp, grm, outname, outname_end);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
      }
#endif

      if (pcp->command_flags1 & kfCommand1WriteSnplist) {
        reterr = WriteSnplist(variant_include, variant_ids, variant_ct, (pcp->misc_flags / kfMiscWriteSnplistZs) & 1, pcp->max_thread_ct, outname, outname_end);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
      }

      if (pcp->command_flags1 & (kfCommand1MakePlink2 | kfCommand1Exportf | kfCommand1WriteCovar)) {
        // If non-null, refalt1_select[j][0] stores the index of the new ref
        // allele and [j][1] stores the index of the new alt1 allele.  (0 =
        // original ref, 1 = original alt1, etc.)
        // Operations which instantiate this (--maj-ref, --ref-allele,
        // --alt1-allele) are only usable with fileset creation commands.  No
        // more pass-marker_reverse-to-everything nonsense.
        // (Technically, I could also drop support for --export, but that would
        // force too many real-world jobs to require two plink2 runs instead of
        // one.)

        const uint32_t setting_alleles_from_file = pcp->ref_allele_flag || pcp->alt1_allele_flag || pcp->fa_fname;
        uint32_t* variant_bps_backup = nullptr;
        const char** allele_storage_backup = nullptr;
        uint32_t max_allele_slen_backup = max_allele_slen;
        uintptr_t* nonref_flags_backup = nullptr;
        uint32_t nonref_flags_was_null = (nonref_flags == nullptr);

        STD_ARRAY_PTR_DECL(AlleleCode, 2, refalt1_select) = nullptr;
        if ((pcp->misc_flags & kfMiscMajRef) || setting_alleles_from_file) {
          const uint32_t need_refalt1_select = ((pcp->misc_flags & kfMiscMajRef) || pcp->ref_allele_flag || pcp->alt1_allele_flag || (pcp->fa_flags & kfFaRefFrom));
          if (loop_cats_idx + 1 < loop_cats_ct) {
            // --ref-allele/--alt1-allele/--normalize may alter
            // allele_storage[] and (in the former two cases) max_allele_slen;
            // --loop-cats doesn't like this.  Save a backup copy.
            if (pcp->ref_allele_flag || pcp->alt1_allele_flag || (pcp->fa_flags & kfFaNormalize)) {
              if (pcp->fa_flags & kfFaNormalize) {
                if (unlikely(bigstack_end_alloc_u32(raw_variant_ct, &variant_bps_backup))) {
                  goto Plink2Core_ret_NOMEM;
                }
                memcpy(variant_bps_backup, variant_bps, raw_variant_ct * sizeof(int32_t));
              }
              if (unlikely(bigstack_end_alloc_kcp(raw_allele_ct, &allele_storage_backup))) {
                goto Plink2Core_ret_NOMEM;
              }
              memcpy(allele_storage_backup, allele_storage, raw_allele_ct * sizeof(intptr_t));
            }
            // nonref_flags may be altered by everything but --normalize.
            if (nonref_flags && need_refalt1_select) {
              if (unlikely(bigstack_end_alloc_w(raw_variant_ctl, &nonref_flags_backup))) {
                goto Plink2Core_ret_NOMEM;
              }
              memcpy(nonref_flags_backup, nonref_flags, raw_variant_ctl * sizeof(intptr_t));
            }
          }
          const uint32_t not_all_nonref = !(pgfi.gflags & kfPgenGlobalAllNonref);
          if (need_refalt1_select) {
            const uintptr_t refalt1_word_ct = DivUp(2 * raw_variant_ct * sizeof(AlleleCode), kBytesPerWord);
            uintptr_t* refalt1_select_ul;
            // no need to track bigstack_end_mark before this is allocated,
            // etc., due to the restriction to --make-pgen/--export
            if (unlikely(bigstack_end_alloc_w(refalt1_word_ct, &refalt1_select_ul))) {
              goto Plink2Core_ret_NOMEM;
            }
            const uintptr_t allele_code_range_size = k1LU << (8 * sizeof(AlleleCode));
            const uintptr_t fill_word = ((~k0LU) / ((allele_code_range_size - 1) * (allele_code_range_size + 1))) * allele_code_range_size;
            for (uintptr_t widx = 0; widx != refalt1_word_ct; ++widx) {
              refalt1_select_ul[widx] = fill_word;
            }
            refalt1_select = R_CAST(STD_ARRAY_PTR_TYPE(AlleleCode, 2), refalt1_select_ul);
            if ((not_all_nonref || setting_alleles_from_file) && (!nonref_flags)) {
              if (unlikely(bigstack_end_alloc_w(raw_variant_ctl, &nonref_flags))) {
                goto Plink2Core_ret_NOMEM;
              }
              pgfi.nonref_flags = nonref_flags;
              // make it clear that this is probably worth cleaning up a bit...
              GET_PRIVATE(simple_pgr, m).fi.nonref_flags = nonref_flags;
              if (not_all_nonref) {
                ZeroWArr(raw_variant_ctl, nonref_flags);
              } else {
                SetAllBits(raw_variant_ct, nonref_flags);
              }
            }
          }
          uintptr_t* previously_seen = nullptr;
          if (pcp->ref_allele_flag) {
            if (pcp->alt1_allele_flag) {
              if (unlikely(bigstack_alloc_w(raw_variant_ctl, &previously_seen))) {
                goto Plink2Core_ret_NOMEM;
              }
            }
            reterr = SetRefalt1FromFile(variant_include, variant_ids, allele_idx_offsets, pcp->ref_allele_flag, raw_variant_ct, variant_ct, max_variant_id_slen, 0, (pcp->misc_flags / kfMiscRefAlleleForce) & 1, pcp->max_thread_ct, allele_storage, &max_allele_slen, refalt1_select, nonref_flags, previously_seen);
            if (unlikely(reterr)) {
              goto Plink2Core_ret_1;
            }
          }
          if (pcp->alt1_allele_flag) {
            reterr = SetRefalt1FromFile(variant_include, variant_ids, allele_idx_offsets, pcp->alt1_allele_flag, raw_variant_ct, variant_ct, max_variant_id_slen, 1, (pcp->misc_flags / kfMiscAlt1AlleleForce) & 1, pcp->max_thread_ct, allele_storage, &max_allele_slen, refalt1_select, nonref_flags, previously_seen);
            if (unlikely(reterr)) {
              goto Plink2Core_ret_1;
            }
            if (previously_seen) {
              BigstackReset(previously_seen);
            }
            // for sanity's sake, --maj-ref, --ref-allele/--alt1-allele, and
            // --ref-from-fa are mutually exclusive
            // (though --ref-from-fa + --alt1-allele may be permitted later)
          } else if (pcp->fa_flags & (kfFaRefFrom | kfFaNormalize)) {
            if (unlikely(vpos_sortstatus & kfUnsortedVarBp)) {
              logerrputs("Error: --normalize and --ref-from-fa require a sorted .pvar/.bim.  Retry this\ncommand after using --make-pgen/--make-bed + --sort-vars to sort your data.\n");
              goto Plink2Core_ret_INCONSISTENT_INPUT;
            }
            reterr = ProcessFa(variant_include, variant_ids, allele_idx_offsets, cip, pcp->fa_fname, max_allele_ct, max_allele_slen, pcp->fa_flags, pcp->max_thread_ct, &vpos_sortstatus, variant_bps, allele_storage, refalt1_select, nonref_flags, outname, outname_end);
            if (unlikely(reterr)) {
              goto Plink2Core_ret_1;
            }
          }
          if (pcp->misc_flags & kfMiscMajRef) {
            // Since this also sets ALT1 to the second-most-common allele, it
            // can't just subscribe to maj_alleles[].
            const uint64_t* main_allele_dosages = nonfounders? allele_dosages : founder_allele_dosages;
            const uint32_t skip_real_ref = not_all_nonref && (!(pcp->misc_flags & kfMiscMajRefForce));
            if (skip_real_ref && (!nonref_flags)) {
              logerrputs("Warning: --maj-ref has no effect, since no provisional reference alleles are\npresent.  (Did you forget to add the 'force' modifier?)\n");
            } else {
              uintptr_t variant_uidx_base = 0;
              uintptr_t cur_bits = variant_include[0];
              for (uint32_t variant_idx = 0; variant_idx != variant_ct; ++variant_idx) {
                const uintptr_t variant_uidx = BitIter1(variant_include, &variant_uidx_base, &cur_bits);
                if (skip_real_ref && IsSet(nonref_flags, variant_uidx)) {
                  continue;
                }
                const uint64_t* cur_allele_dosages = &(main_allele_dosages[allele_idx_offsets? allele_idx_offsets[variant_uidx] : (2 * variant_uidx)]);
                const uint32_t allele_ct = allele_idx_offsets? (allele_idx_offsets[variant_uidx + 1] - allele_idx_offsets[variant_uidx]) : 2;
                if (allele_ct == 2) {
                  // optimize common case: only make one assignment
                  if (cur_allele_dosages[1] > cur_allele_dosages[0]) {
                    R_CAST(DoubleAlleleCode*, refalt1_select)[variant_uidx] = 1;
                    if (nonref_flags) {
                      SetBit(variant_uidx, nonref_flags);
                    }
                  }
                } else {
                  uint32_t new_ref_idx = (cur_allele_dosages[1] > cur_allele_dosages[0]);
                  uint32_t new_alt1_idx = 1 - new_ref_idx;
                  uint64_t ref_dosage = cur_allele_dosages[new_ref_idx];
                  uint64_t alt1_dosage = cur_allele_dosages[new_alt1_idx];
                  for (uint32_t alt_idx = 2; alt_idx != allele_ct; ++alt_idx) {
                    const uint64_t cur_alt_dosage = cur_allele_dosages[alt_idx];
                    if (cur_alt_dosage > alt1_dosage) {
                      if (cur_alt_dosage > ref_dosage) {
                        alt1_dosage = ref_dosage;
                        ref_dosage = cur_alt_dosage;
                        new_alt1_idx = new_ref_idx;
                        new_ref_idx = alt_idx;
                      } else {
                        alt1_dosage = cur_alt_dosage;
                        new_alt1_idx = alt_idx;
                      }
                    }
                  }
                  if (new_ref_idx || (new_alt1_idx != 1)) {
                    refalt1_select[variant_uidx][0] = new_ref_idx;
                    refalt1_select[variant_uidx][1] = new_alt1_idx;
                    if (nonref_flags) {
                      SetBit(variant_uidx, nonref_flags);
                    }
                  }
                }
              }
            }
          }
        }
        BigstackReset(bigstack_mark_allele_dosages);

        uint32_t* new_sample_idx_to_old = nullptr;
        if (pcp->sample_sort_flags & (kfSortNatural | kfSortAscii | kfSortFile)) {
          if (sample_ct < 2) {
            logerrputs("Warning: Skipping --sample-sort since <2 samples are present.\n");
          } else {
            if (pcp->sample_sort_flags & kfSortFile) {
              reterr = SampleSortFileMap(sample_include, &pii.sii, pcp->sample_sort_fname, raw_sample_ct, sample_ct, &new_sample_idx_to_old);
              if (unlikely(reterr)) {
                goto Plink2Core_ret_1;
              }
            } else {
              // probably more efficient to have --make-{bed,pgen,bpgen}
              // perform an unfiltered load?  but we should have compute power
              // to spare here, so keep the code simpler for now
              char* sorted_xidbox_tmp;
              uintptr_t max_xid_blen;
              reterr = SortedXidboxInitAlloc(sample_include, &pii.sii, sample_ct, 0, pii.sii.sids? kfXidModeFidIidSid : kfXidModeFidIid, (pcp->sample_sort_flags == kfSortNatural), &sorted_xidbox_tmp, &new_sample_idx_to_old, &max_xid_blen);
              if (unlikely(reterr)) {
                goto Plink2Core_ret_1;
              }
              BigstackReset(sorted_xidbox_tmp);
            }
            logprintf("--indiv-sort: %u samples reordered.\n", sample_ct);
          }
        }

        // update (18 Mar 2018): permit no-covariate --write-covar when used to
        // write phenotypes.
        if ((covar_ct || (pheno_ct && (pcp->write_covar_flags & (kfWriteCovarColPheno1 | kfWriteCovarColPhenos)))) && ((pcp->command_flags1 & (kfCommand1Exportf | kfCommand1WriteCovar)) || ((pcp->command_flags1 & kfCommand1MakePlink2) && (make_plink2_flags & (kfMakeBed | kfMakeFam | kfMakePgen | kfMakePsam))))) {
          reterr = WriteCovar(sample_include, &pii, sex_nm, sex_male, pheno_cols, pheno_names, covar_cols, covar_names, new_sample_idx_to_old, sample_ct, pheno_ct, max_pheno_name_blen, covar_ct, max_covar_name_blen, pcp->write_covar_flags, outname, outname_end);
          if (unlikely(reterr)) {
            goto Plink2Core_ret_1;
          }
        } else if (pcp->command_flags1 & kfCommand1WriteCovar) {
          if (pcp->write_covar_flags & (kfWriteCovarColPheno1 | kfWriteCovarColPhenos)) {
            logerrputs("Warning: Skipping --write-covar, since no phenotypes or covariates are loaded.\n");
          } else {
            logerrputs("Warning: Skipping --write-covar, since no covariates are loaded.\n");
          }
        }

        if (pcp->command_flags1 & kfCommand1MakePlink2) {
          // todo: unsorted case (--update-chr, etc.)
          if (pcp->sort_vars_flags != kfSort0) {
            reterr = MakePlink2Vsort(xheader, sample_include, &pii, sex_nm, sex_male, pheno_cols, pheno_names, new_sample_idx_to_old, variant_include, cip, variant_bps, variant_ids, allele_idx_offsets, allele_storage, allele_presents, refalt1_select, pvar_qual_present, pvar_quals, pvar_filter_present, pvar_filter_npass, pvar_filter_storage, info_reload_slen? pvarname : nullptr, variant_cms, chr_idxs, xheader_blen, info_flags, raw_sample_ct, sample_ct, pheno_ct, max_pheno_name_blen, raw_variant_ct, variant_ct, max_allele_ct, max_allele_slen, max_filter_slen, info_reload_slen, pcp->max_thread_ct, pcp->hard_call_thresh, pcp->dosage_erase_thresh, make_plink2_flags, (pcp->sort_vars_flags == kfSortNatural), pcp->pvar_psam_flags, &simple_pgr, outname, outname_end);
          } else {
            if (vpos_sortstatus & kfUnsortedVarBp) {
              logerrputs("Warning: Variants are not sorted by position.  Consider rerunning with the\n--sort-vars flag added to remedy this.\n");
            }
            reterr = MakePlink2NoVsort(xheader, sample_include, &pii, sex_nm, sex_male, pheno_cols, pheno_names, new_sample_idx_to_old, variant_include, cip, variant_bps, variant_ids, allele_idx_offsets, allele_storage, allele_presents, refalt1_select, pvar_qual_present, pvar_quals, pvar_filter_present, pvar_filter_npass, pvar_filter_storage, info_reload_slen? pvarname : nullptr, variant_cms, pcp->varid_template_str, pcp->varid_multi_template_str, pcp->varid_multi_nonsnp_template_str, pcp->missing_varid_match, xheader_blen, info_flags, raw_sample_ct, sample_ct, pheno_ct, max_pheno_name_blen, raw_variant_ct, variant_ct, max_allele_ct, max_allele_slen, max_filter_slen, info_reload_slen, pcp->max_thread_ct, pcp->hard_call_thresh, pcp->dosage_erase_thresh, pcp->new_variant_id_max_allele_slen, pcp->misc_flags, make_plink2_flags, pcp->pvar_psam_flags, pgr_alloc_cacheline_ct, &pgfi, &simple_pgr, outname, outname_end);
          }
          if (unlikely(reterr)) {
            goto Plink2Core_ret_1;
          }
          // no BigstackReset needed here, since allele_presents only needed
          // if 'trim-alts', and later operations are prohibited in that case
        }

        if (pcp->command_flags1 & kfCommand1Exportf) {
          reterr = Exportf(sample_include, &pii, sex_nm, sex_male, pheno_cols, pheno_names, variant_include, cip, variant_bps, variant_ids, allele_idx_offsets, allele_storage, refalt1_select, pvar_qual_present, pvar_quals, pvar_filter_present, pvar_filter_npass, pvar_filter_storage, info_reload_slen? pvarname : nullptr, variant_cms, &(pcp->exportf_info), xheader_blen, info_flags, raw_sample_ct, sample_ct, pheno_ct, max_pheno_name_blen, raw_variant_ct, variant_ct, max_variant_id_slen, max_allele_slen, max_filter_slen, info_reload_slen, pcp->max_thread_ct, make_plink2_flags, pgr_alloc_cacheline_ct, xheader, &pgfi, &simple_pgr, outname, outname_end);
          if (unlikely(reterr)) {
            goto Plink2Core_ret_1;
          }
        }

        if (variant_bps_backup) {
          memcpy(variant_bps, variant_bps_backup, raw_variant_ct * sizeof(int32_t));
        }
        if (allele_storage_backup) {
          max_allele_slen = max_allele_slen_backup;
          memcpy(allele_storage, allele_storage_backup, raw_allele_ct * sizeof(intptr_t));
        }
        if (nonref_flags_backup) {
          memcpy(nonref_flags, nonref_flags_backup, raw_variant_ctl * sizeof(intptr_t));
        } else if (nonref_flags_was_null) {
          nonref_flags = nullptr;
          pgfi.nonref_flags = nullptr;
          GET_PRIVATE(simple_pgr, m).fi.nonref_flags = nullptr;
        }
      }
      BigstackReset(bigstack_mark_allele_dosages);

      if (pcp->command_flags1 & kfCommand1LdPrune) {
        if (unlikely((pcp->ld_info.prune_flags & kfLdPruneWindowBp) && (vpos_sortstatus & kfUnsortedVarBp))) {
          logerrputs("Error: When the window size is in kb units, LD-based pruning requires a sorted\n.pvar/.bim.  Retry this command after using --make-pgen/--make-bed +\n--sort-vars to sort your data.\n");
          goto Plink2Core_ret_INCONSISTENT_INPUT;
        }
        reterr = LdPrune(variant_include, cip, variant_bps, variant_ids, allele_idx_offsets, maj_alleles, allele_freqs, founder_info, sex_male, &(pcp->ld_info), raw_variant_ct, variant_ct, raw_sample_ct, founder_ct, pcp->max_thread_ct, &simple_pgr, outname, outname_end);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
      }

      if (pcp->command_flags1 & kfCommand1Ld) {
        reterr = LdConsole(variant_include, cip, variant_ids, allele_idx_offsets, maj_alleles, allele_storage, founder_info, sex_nm, sex_male, &(pcp->ld_info), variant_ct, raw_sample_ct, founder_ct, &simple_pgr);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
      }

      if (pcp->command_flags1 & kfCommand1Score) {
        reterr = ScoreReport(sample_include, &pii.sii, sex_male, pheno_cols, pheno_names, variant_include, cip, variant_ids, allele_idx_offsets, allele_storage, allele_freqs, &(pcp->score_info), raw_sample_ct, sample_ct, pheno_ct, max_pheno_name_blen, raw_variant_ct, variant_ct, max_variant_id_slen, pcp->xchr_model, pcp->max_thread_ct, &simple_pgr, outname, outname_end);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
      }
      if (pcp->command_flags1 & kfCommand1Vscore) {
        reterr = Vscore(variant_include, cip, variant_bps, variant_ids, allele_idx_offsets, allele_storage, sample_include, &pii.sii, sex_male, allele_freqs, pcp->vscore_fname, &(pcp->vscore_col_idx_range_list), raw_variant_ct, variant_ct, raw_sample_ct, sample_ct, max_allele_slen, pcp->vscore_flags, pcp->xchr_model, pcp->max_thread_ct, pgr_alloc_cacheline_ct, &pgfi, outname, outname_end);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
      }
      // eventually check for nonzero pheno_ct here?

      if (pcp->command_flags1 & kfCommand1Glm) {
        reterr = GlmMain(sample_include, &pii.sii, sex_nm, sex_male, pheno_cols, pheno_names, covar_cols, covar_names, variant_include, cip, variant_bps, variant_ids, allele_idx_offsets, maj_alleles, allele_storage, &(pcp->glm_info), &(pcp->adjust_info), &(pcp->aperm), pcp->glm_local_covar_fname, pcp->glm_local_pvar_fname, pcp->glm_local_psam_fname, raw_sample_ct, sample_ct, pheno_ct, max_pheno_name_blen, covar_ct, max_covar_name_blen, raw_variant_ct, variant_ct, max_variant_id_slen, max_allele_slen, pcp->xchr_model, pcp->ci_size, pcp->vif_thresh, pcp->ln_pfilter, pcp->output_min_ln, pcp->max_thread_ct, pgr_alloc_cacheline_ct, &pgfi, &simple_pgr, outname, outname_end);
        if (unlikely(reterr)) {
          goto Plink2Core_ret_1;
        }
      }
    }
  }
  while (0) {
  Plink2Core_ret_NOMEM:
    reterr = kPglRetNomem;
    break;
  Plink2Core_ret_OPEN_FAIL:
    reterr = kPglRetOpenFail;
    break;
  Plink2Core_ret_INVALID_CMDLINE:
    reterr = kPglRetInvalidCmdline;
    break;
  Plink2Core_ret_MALFORMED_INPUT:
    reterr = kPglRetMalformedInput;
    break;
  Plink2Core_ret_INCONSISTENT_INPUT:
    reterr = kPglRetInconsistentInput;
    break;
  Plink2Core_ret_DEGENERATE_DATA:
    reterr = kPglRetDegenerateData;
    break;
  }
 Plink2Core_ret_1:
  if (loop_cats_pheno_col) {
    vecaligned_free_cond(loop_cats_pheno_col->nonmiss);
  }
  CleanupPhenoCols(covar_ct, covar_cols);
  CleanupPhenoCols(pheno_ct, pheno_cols);
  free_cond(covar_names);
  free_cond(pheno_names);
  CleanupPgr2(".pgen file", &simple_pgr, &reterr);
  CleanupPgfi2(".pgen file", &pgfi, &reterr);
  // no BigstackReset() needed?
  return reterr;
}

PglErr ZstDecompress(const char* in_fname, const char* out_fname) {
  zstRFILE zrf;
  PreinitZstRfile(&zrf);
  FILE* outfile = nullptr;
  PglErr reterr = kPglRetSuccess;
  {
    reterr = ZstRfileOpen(in_fname, &zrf);
    if (unlikely(reterr)) {
      if (reterr == kPglRetNomem) {
        goto ZstDecompress_ret_NOMEM;
      }
      if (reterr == kPglRetOpenFail) {
        fprintf(stderr, kErrprintfFopen, in_fname, strerror(errno));
        goto ZstDecompress_ret_OPEN_FAIL;
      }
      goto ZstDecompress_ret_RFILE_FAIL;
    }
    if (out_fname) {
      outfile = fopen(out_fname, FOPEN_WB);
      if (unlikely(!outfile)) {
        fprintf(stderr, kErrprintfFopen, out_fname, strerror(errno));
        goto ZstDecompress_ret_OPEN_FAIL;
      }
    } else {
      outfile = stdout;
    }
    unsigned char* buf = R_CAST(unsigned char*, g_textbuf);
    while (1) {
      const int32_t bytes_read = zstread(&zrf, buf, kTextbufMainSize);
      if (bytes_read <= 0) {
        if (likely(!bytes_read)) {
          break;
        }
        reterr = ZstRfileErrcode(&zrf);
        goto ZstDecompress_ret_RFILE_FAIL;
      }
      if (unlikely(!fwrite_unlocked(buf, bytes_read, 1, outfile))) {
        goto ZstDecompress_ret_WRITE_FAIL;
      }
      fflush(outfile);
    }
    if (out_fname) {
      if (unlikely(fclose_null(&outfile))) {
        goto ZstDecompress_ret_WRITE_FAIL;
      }
    }
  }
  // we exit from main() immediately, so need to print nomem/read/write error
  // messages here
  while (0) {
  ZstDecompress_ret_NOMEM:
    // in this exceedingly unlikely case, the --memory flag doesn't help, so
    // print a different message
    fputs("Error: Out of memory.\n", stderr);
    reterr = kPglRetNomem;
    break;
  ZstDecompress_ret_OPEN_FAIL:
    reterr = kPglRetOpenFail;
    break;
  ZstDecompress_ret_RFILE_FAIL:
    if (reterr == kPglRetReadFail) {
      fprintf(stderr, kErrprintfFread, in_fname, zsterror(&zrf));
    } else {
      fprintf(stderr, kErrprintfDecompress, in_fname, zsterror(&zrf));
    }
    break;
  ZstDecompress_ret_WRITE_FAIL:
    fprintf(stderr, kErrstrWrite, strerror(errno));
    reterr = kPglRetWriteFail;
    break;
  }
  if (out_fname) {
    fclose_cond(outfile);
  }
  if (unlikely(CleanupZstRfile(&zrf, &reterr))) {
    fprintf(stderr, kErrprintfFread, in_fname, strerror(errno));
  }
  return reterr;
}

PglErr Alloc2col(const char* const* sources, const char* flagname_p, uint32_t param_ct, TwoColParams** tcbuf) {
  uint32_t fname_blen = strlen(sources[0]) + 1;
  if (unlikely(fname_blen > kPglFnamesize)) {
    logerrprintf("Error: --%s filename too long.\n", flagname_p);
    return kPglRetOpenFail;
  }
  if (unlikely(pgl_malloc(offsetof(TwoColParams, fname) + fname_blen, tcbuf))) {
    return kPglRetNomem;
  }
  memcpy((*tcbuf)->fname, sources[0], fname_blen);
  (*tcbuf)->skip_ct = 0;
  (*tcbuf)->skipchar = '\0';
  if (param_ct > 1) {
    if (unlikely(ScanPosintDefcapx(sources[1], &((*tcbuf)->colx)))) {
      logerrprintf("Error: Invalid --%s column number.\n", flagname_p);
      return kPglRetInvalidCmdline;
    }
    if (param_ct > 2) {
      if (unlikely(ScanPosintDefcapx(sources[2], &((*tcbuf)->colid)))) {
        logerrprintf("Error: Invalid --%s variant ID column number.\n", flagname_p);
        return kPglRetInvalidCmdline;
      }
      if (param_ct == 4) {
        char cc = sources[3][0];
        if ((cc < '0') || (cc > '9')) {
          cc = ExtractCharParam(sources[3]);
          if (unlikely(!cc)) {
            goto Alloc2col_invalid_skip;
          }
          (*tcbuf)->skipchar = cc;
        } else {
          if (unlikely(ScanUintDefcapx(sources[3], &((*tcbuf)->skip_ct)))) {
          Alloc2col_invalid_skip:
            logerrprintf("Error: Invalid --%s skip parameter.  This needs to either be a\nsingle character (usually '#') which, when present at the start of a line,\nindicates it should be skipped; or the number of initial lines to skip.  (Note\nthat in shells such as bash, '#' is a special character that must be\nsurrounded by single- or double-quotes to be parsed correctly.)\n", flagname_p);
            return kPglRetInvalidCmdline;
          }
        }
      }
    } else {
      (*tcbuf)->colid = 1;
    }
    if (unlikely((*tcbuf)->colx == (*tcbuf)->colid)) {
      logerrprintf("Error: Column numbers for --%s cannot be equal.\n%s", flagname_p, errstr_append);
      return kPglRetInvalidCmdline;
    }
  } else {
    (*tcbuf)->colx = 2;
    (*tcbuf)->colid = 1;
  }
  return kPglRetSuccess;
}

PglErr AllocAndFlattenCommaDelim(const char* const* sources, uint32_t param_ct, char** flattened_buf_ptr) {
  uint32_t tot_blen = 1;
  for (uint32_t param_idx = 0; param_idx != param_ct; ++param_idx) {
    const char* cur_param_iter = sources[param_idx];
    while (1) {
      while (*cur_param_iter == ',') {
        ++cur_param_iter;
      }
      const char* cur_token_end = strchr(cur_param_iter, ',');
      if (!cur_token_end) {
        break;
      }
      tot_blen += 1 + S_CAST(uintptr_t, cur_token_end - cur_param_iter);
      cur_param_iter = &(cur_token_end[1]);
    }
    tot_blen += 1 + strlen(cur_param_iter);
  }
  char* write_iter;
  if (unlikely(pgl_malloc(tot_blen, &write_iter))) {
    return kPglRetNomem;
  }
  *flattened_buf_ptr = write_iter;
  for (uint32_t param_idx = 0; param_idx != param_ct; ++param_idx) {
    const char* cur_param_iter = sources[param_idx];
    while (1) {
      while (*cur_param_iter == ',') {
        ++cur_param_iter;
      }
      const char* cur_token_end = strchr(cur_param_iter, ',');
      if (!cur_token_end) {
        break;
      }
      write_iter = memcpyax(write_iter, cur_param_iter, cur_token_end - cur_param_iter, '\0');
      cur_param_iter = &(cur_token_end[1]);
    }
    write_iter = strcpyax(write_iter, cur_param_iter, '\0');
  }
  *write_iter = '\0';
  return kPglRetSuccess;
}

void PrintVer() {
  fputs(ver_str, stdout);
  fputs(ver_str2, stdout);
}

uint32_t CmdlineSingleChr(const ChrInfo* cip, MiscFlags misc_flags) {
  if ((misc_flags & (kfMiscAutosomeOnly | kfMiscAutosomePar)) || (!cip->is_include_stack)) {
    return 0;
  }
  const uint32_t main_chr_ct = PopcountWords(cip->chr_mask, kChrExcludeWords) + PopcountWord(cip->chr_mask[kChrMaskWords - 1]);
  if (main_chr_ct > 1) {
    return 0;
  }
  if (main_chr_ct == 1) {
    return (cip->incl_excl_name_stack == nullptr);
  }
  return cip->incl_excl_name_stack && (!(cip->incl_excl_name_stack->next));
}

void GetExportfTargets(const char* const* argvk, uint32_t param_ct, ExportfFlags* exportf_flags_ptr, IdpasteFlags* exportf_id_paste_ptr, uint32_t* format_param_idxs_ptr) {
  // does not error out if no format present, since this is needed for --recode
  // translation
  // supports multiple formats
  uint32_t format_param_idxs = 0;
  for (uint32_t param_idx = 1; param_idx <= param_ct; ++param_idx) {
    const char* cur_modif = argvk[param_idx];
    const char* cur_modif2 = &(cur_modif[1]);
    ExportfFlags cur_format = kfExportf0;
    switch (*cur_modif) {
    case '2':
      if (!strcmp(cur_modif2, "3")) {
        cur_format = kfExportf23;
      }
      break;
    case 'A':
    case 'a':
      if (!cur_modif2[0]) {
        cur_format = kfExportfA;
      } else if (((cur_modif2[0] & 0xdf) == 'D') && (!cur_modif2[1])) {
        cur_format = kfExportfAD;
      } else if (!strcmp(cur_modif2, "-transpose")) {
        cur_format = kfExportfATranspose;
      }
      break;
    case 'b':
      {
        const uint32_t cur_modif2_slen = strlen(cur_modif2);
        if (strequal_k(cur_modif2, "eagle", cur_modif2_slen)) {
          cur_format = kfExportfBeagle;
        } else if (strequal_k(cur_modif2, "eagle-nomap", cur_modif2_slen)) {
          cur_format = kfExportfBeagleNomap;
        } else if (strequal_k(cur_modif2, "gen-1.1", cur_modif2_slen) ||
                   strequal_k(cur_modif2, "gen_1.1", cur_modif2_slen)) {
          cur_format = kfExportfBgen11;
        } else if (strequal_k(cur_modif2, "gen-1.2", cur_modif2_slen) ||
                   strequal_k(cur_modif2, "gen_1.2", cur_modif2_slen)) {
          cur_format = kfExportfBgen12;
        } else if (strequal_k(cur_modif2, "gen-1.3", cur_modif2_slen) ||
                   strequal_k(cur_modif2, "gen_1.3", cur_modif2_slen)) {
          cur_format = kfExportfBgen13;
        } else if (strequal_k(cur_modif2, "imbam", cur_modif2_slen)) {
          cur_format = kfExportfBimbam;
        } else if (strequal_k(cur_modif2, "imbam-1chr", cur_modif2_slen)) {
          cur_format = kfExportfBimbam1chr;
        }
        break;
      }
    case 'c':
      if (!strcmp(cur_modif2, "ompound-genotypes")) {
        cur_format = kfExportfCompound;
      }
      break;
    case 'f':
      if (!strcmp(cur_modif2, "astphase")) {
        cur_format = kfExportfFastphase;
      } else if (!strcmp(cur_modif2, "astphase-1chr")) {
        cur_format = kfExportfFastphase1chr;
      }
      break;
    case 'h':
      if (!strcmp(cur_modif2, "aps")) {
        cur_format = kfExportfHaps;
        break;
      } else if (!strcmp(cur_modif2, "apslegend")) {
        cur_format = kfExportfHapsLegend;
        break;
      }
      // fall through
    case 'H':
      if ((cur_modif2[0] & 0xdf) == 'V') {
        if (!cur_modif2[1]) {
          cur_format = kfExportfHv;
        } else if (!strcmp(&(cur_modif2[1]), "-1chr")) {
          cur_format = kfExportfHv1chr;
        }
      }
      break;
    case 'i':
      if (!strcmp(cur_modif2, "nd-major-bed")) {
        cur_format = kfExportfIndMajorBed;
      }
      break;
    case 'l':
      {
        const uint32_t cur_modif2_slen = strlen(cur_modif2);
        if (strequal_k(cur_modif2, "gen", cur_modif2_slen)) {
          cur_format = kfExportfLgen;
        } else if (strequal_k(cur_modif2, "gen-ref", cur_modif2_slen)) {
          cur_format = kfExportfLgenRef;
        } else if (strequal_k(cur_modif2, "ist", cur_modif2_slen)) {
          cur_format = kfExportfList;
        }
        break;
      }
    case 'o':
      if (!strcmp(cur_modif2, "xford")) {
        cur_format = kfExportfOxGen;
      }
      break;
    case 'p':
      if (!strcmp(cur_modif2, "ed")) {
        cur_format = kfExportfPed;
      }
      break;
    case 'r':
      if (!strcmp(cur_modif2, "list")) {
        cur_format = kfExportfRlist;
      }
      break;
    case 's':
      if (!strcmp(cur_modif2, "tructure")) {
        cur_format = kfExportfStructure;
      }
      break;
    case 't':
      if (!strcmp(cur_modif2, "ranspose")) {
        cur_format = kfExportfTranspose;
      }
      break;
    case 'v':
      if ((cur_modif2[0] == 'c') && (cur_modif2[1] == 'f')) {
        if (!cur_modif2[2]) {
          cur_format = kfExportfVcf43;
        } else if (!strcmp(&(cur_modif2[2]), "-4.2")) {
          cur_format = kfExportfVcf42;
        } else if ((!strcmp(&(cur_modif2[2]), "-fid")) || (!strcmp(&(cur_modif2[2]), "-iid"))) {
          snprintf(g_logbuf, kLogbufSize, "Note: --export 'v%s' modifier is deprecated.  Use 'vcf' + 'id-paste=%s'.\n", cur_modif2, &(cur_modif2[3]));
          cur_format = kfExportfVcf43;
          *exportf_id_paste_ptr = (cur_modif2[3] == 'f')? kfIdpasteFid : kfIdpasteIid;
        }
      }
      break;
    }
    if (cur_format) {
      format_param_idxs |= 1U << param_idx;
      *exportf_flags_ptr |= cur_format;
    }
  }
  *format_param_idxs_ptr = format_param_idxs;
}

uint32_t VaridTemplateIsValid(const char* varid_str, const char* flagname_p) {
  const char* sptr = strchr(varid_str, '@');
  const char* sptr2 = strchr(varid_str, '#');
  if (unlikely((!sptr) || (!sptr2) || strchr(&(sptr[1]), '@') || strchr(&(sptr2[1]), '#'))) {
    logerrprintfww("Error: The --%s template string requires exactly one '@' and one '#'.\n", flagname_p);
    return 0;
  }
  // snp/nonsnp is not sufficient for assigning unique IDs to unnamed 1000
  // Genomes phase 3 variants (see e.g. chr22:18078898).  So we now allow the
  // template string to include allele names, where '$r' = reference allele,
  // '$a' = alt1, and '$1'/'$2' refer to ref/alt1 in ASCII-sort order
  // (necessary for interoperation with plink1).
  // For now, either '$' must be entirely absent from the template string, or
  // '$r' and/or '$a' appear exactly once, or '$1' and '$2' both appear exactly
  // once.
  // probable todo: alternate naming scheme for long indels (e.g. first base,
  //   middle length, last base, like "i18n")
  // possible todo: some way to include alt2, etc. in name
  sptr = strchr(varid_str, '$');
  if (sptr) {
    sptr2 = &(sptr[1]);
    uint32_t first_allele_type_code = ctou32(*sptr2);
    if ((first_allele_type_code == 49) || (first_allele_type_code == 50)) {
      sptr2 = strchr(sptr2, '$');
      if (unlikely((!sptr2) || strchr(&(sptr2[1]), '$') || ((first_allele_type_code + ctou32(sptr2[1])) != 99))) {
      VaridTemplateIsValid_dollar_error:
        logerrprintfww("Error: The --%s template string requires either no instances of '$', exactly one instance of '$r' and/or '$a', or exactly one '$1' and one '$2'.\n", flagname_p);
        return 0;
      }
    } else {
      first_allele_type_code &= 0xdf;
      if (unlikely((first_allele_type_code != 65) && (first_allele_type_code != 82))) {
        goto VaridTemplateIsValid_dollar_error;
      }
      sptr2 = strchr(sptr2, '$');
      if (sptr2) {
        const uint32_t second_allele_type_code = ctou32(*(++sptr2)) & 0xdf;
        if (unlikely(((first_allele_type_code + second_allele_type_code) != 147) || strchr(sptr2, '$'))) {
          goto VaridTemplateIsValid_dollar_error;
        }
      }
    }
  }
  return 1;
}

BoolErr ParseFreqSelector(const char* mode_str, const char* flagname_p, FreqFilterMode* modep) {
  // assume preinitialization to kFreqFilterNonmajor
  if (mode_str[0] == ':') {
    ++mode_str;
  }
  const uint32_t mode_slen = strlen(mode_str);
  if (strequal_k(mode_str, "nref", mode_slen)) {
    *modep = kFreqFilterNref;
  } else if (strequal_k(mode_str, "alt1", mode_slen)) {
    *modep = kFreqFilterAlt1;
  } else if (strequal_k(mode_str, "minor", mode_slen)) {
    *modep = kFreqFilterMinor;
  } else if (unlikely(!strequal_k(mode_str, "nonmajor", mode_slen))) {
    snprintf(g_logbuf, kLogbufSize, "Error: Invalid --%s mode '%s'.\n", flagname_p, mode_str);
    return 1;
  }
  return 0;
}


static_assert(sizeof(int) == sizeof(int32_t), "main() assumes int and int32_t are synonymous.");
static_assert(!kChrOffsetX, "--autosome-num/--chr-set/--cow/etc. assume kChrOffsetX == 0.");
static_assert(kChrOffsetY == 1, "--chr-set/--cow/... assume kChrOffsetY == 1.");
static_assert(kChrOffsetXY == 2, "--chr-set/--cow/... assume kChrOffsetXY == 2.");
static_assert(kChrOffsetMT == 3, "--chr-set/--cow/... assume kChrOffsetMT == 3.");
#ifdef __cplusplus
}  // namespace plink2
#endif

#if defined(CPU_CHECK_SSE42) || defined(CPU_CHECK_AVX2)
int RealMain(int argc, char** argv) {
#else
int main(int argc, char** argv) {
#endif
#ifdef __cplusplus
  using namespace plink2;
#endif

#ifdef __APPLE__
  fesetenv(FE_DFL_DISABLE_SSE_DENORMS_ENV);
#else
#  ifdef __LP64__
  _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
#  endif
#endif
  // special case, since it may dump to stdout
  if (argc > 1) {
    const char* argv1 = argv[1];
    const uint32_t argv1_slen = strlen(argv1);
    if (strequal_k(argv1, "--zst-decompress", argv1_slen) ||
        strequal_k(argv1, "-zst-decompress", argv1_slen) ||
        strequal_k(argv1, "--zd", argv1_slen) ||
        strequal_k(argv1, "-zd", argv1_slen)) {
      if (unlikely(argc == 2)) {
        fprintf(stderr, "Error: Missing %s parameter.\n", argv[1]);
        return S_CAST(uint32_t, kPglRetInvalidCmdline);
      }
      for (int ii = 2; ii != argc; ++ii) {
        if (unlikely(IsCmdlineFlag(argv[S_CAST(uint32_t, ii)]))) {
          fprintf(stderr, "Error: %s cannot be used with other flags.\n", argv[1]);
          return S_CAST(uint32_t, kPglRetInvalidCmdline);
        }
      }
      if (unlikely(argc > 4)) {
        fprintf(stderr, "Error: %s accepts at most 2 parameters.\n", argv[1]);
        return S_CAST(uint32_t, kPglRetInvalidCmdline);
      }
      return S_CAST(uint32_t, ZstDecompress(argv[2], (argc == 4)? argv[3] : nullptr));
    }
  }

  unsigned char* bigstack_ua = nullptr;
  Plink2CmdlineMeta pcm;
  PreinitPlink2CmdlineMeta(&pcm);
  const char* flagname_p = nullptr;
  char* king_cutoff_fprefix = nullptr;
  char* const_fid = nullptr;
  char* import_single_chr_str = nullptr;
  char* ox_missing_code = nullptr;
  char* vcf_dosage_import_field = nullptr;
  uint32_t* rseeds = nullptr;
  LlStr* file_delete_list = nullptr;
  uint32_t arg_idx = 0;
  uint32_t print_end_time = 0;
  uint32_t warning_errcode = 0;
  PglErr reterr = kPglRetSuccess;
  Plink2Cmdline pc;
  pc.filter_flags = kfFilter0;
  pc.dependency_flags = kfFilter0;
  pc.var_filter_exceptions_flattened = nullptr;
  pc.varid_template_str = nullptr;
  pc.varid_multi_template_str = nullptr;
  pc.varid_multi_nonsnp_template_str = nullptr;
  pc.missing_varid_match = nullptr;
  pc.varid_from = nullptr;
  pc.varid_to = nullptr;
  pc.varid_snp = nullptr;
  pc.varid_exclude_snp = nullptr;
  pc.pheno_fname = nullptr;
  pc.covar_fname = nullptr;
  pc.sample_sort_fname = nullptr;
  pc.keep_fnames = nullptr;
  pc.keepfam_fnames = nullptr;
  pc.remove_fnames = nullptr;
  pc.removefam_fnames = nullptr;
  pc.extract_fnames = nullptr;
  pc.extract_intersect_fnames = nullptr;
  pc.exclude_fnames = nullptr;
  pc.freq_ref_binstr = nullptr;
  pc.freq_alt1_binstr = nullptr;
  pc.glm_local_covar_fname = nullptr;
  pc.glm_local_pvar_fname = nullptr;
  pc.glm_local_psam_fname = nullptr;
  pc.read_freq_fname = nullptr;
  pc.within_fname = nullptr;
  pc.catpheno_name = nullptr;
  pc.family_missing_catname = nullptr;
  pc.keep_cats_fname = nullptr;
  pc.keep_cat_names_flattened = nullptr;
  pc.keep_cat_phenoname = nullptr;
  pc.remove_cats_fname = nullptr;
  pc.remove_cat_names_flattened = nullptr;
  pc.remove_cat_phenoname = nullptr;
  pc.split_cat_phenonames_flattened = nullptr;
  pc.require_pheno_flattened = nullptr;
  pc.require_covar_flattened = nullptr;
  pc.vstd_flattened = nullptr;
  pc.quantnorm_flattened = nullptr;
  pc.covar_quantnorm_flattened = nullptr;
  pc.loop_cats_phenoname = nullptr;
  pc.fa_fname = nullptr;
  pc.king_table_subset_fname = nullptr;
  pc.require_info_flattened = nullptr;
  pc.require_no_info_flattened = nullptr;
  pc.keep_fcol_fname = nullptr;
  pc.keep_fcol_flattened = nullptr;
  pc.keep_fcol_name = nullptr;
  pc.update_alleles_fname = nullptr;
  pc.ref_allele_flag = nullptr;
  pc.alt1_allele_flag = nullptr;
  pc.update_map_flag = nullptr;
  pc.update_name_flag = nullptr;
  pc.update_sample_ids_fname = nullptr;
  pc.update_parental_ids_fname = nullptr;
  pc.recover_var_ids_fname = nullptr;
  pc.vscore_fname = nullptr;
  InitRangeList(&pc.snps_range_list);
  InitRangeList(&pc.exclude_snps_range_list);
  InitRangeList(&pc.pheno_range_list);
  InitRangeList(&pc.covar_range_list);
  InitRangeList(&pc.vscore_col_idx_range_list);
  InitUpdateSex(&pc.update_sex_info);
  InitLd(&pc.ld_info);
  InitSdiff(&pc.sdiff_info);
  InitGlm(&pc.glm_info);
  InitScore(&pc.score_info);
  InitCmpExpr(&pc.keep_if_expr);
  InitCmpExpr(&pc.remove_if_expr);
  InitCmpExpr(&pc.extract_if_info_expr);
  InitCmpExpr(&pc.exclude_if_info_expr);
  InitExtractFcol(&pc.extract_fcol_info);
  InitExportf(&pc.exportf_info);
  AdjustFileInfo adjust_file_info;
  InitAdjust(&pc.adjust_info, &adjust_file_info);
  ChrInfo chr_info;
  if (unlikely(InitChrInfo(&chr_info))) {
    goto main_ret_NOMEM_NOLOG;
  }

  {
    // standardize strtod() behavior
    // (not necessary any more since we use dtoa_g() instead)
    // setlocale(LC_NUMERIC, "C");

    uint32_t first_arg_idx;
    uint32_t flag_ct;
    reterr = CmdlineParsePhase1(ver_str, ver_str2, PROG_NAME_STR, notestr_null_calc2, kCmdlineFormatStr, errstr_append, kMaxFlagBlen, DispHelp, &argc, &argv, &pcm, &first_arg_idx, &flag_ct);
    if (reterr) {
      if (reterr == kPglRetHelp) {
        reterr = kPglRetSuccess;
      }
      goto main_ret_NOLOG;
    }
    if (!flag_ct) {
      goto main_ret_NULL_CALC_0;
    }
    // + (kBytesPerWord - 1) to support overread during sort
    if (unlikely(
            pgl_malloc(flag_ct * kMaxFlagBlen + kBytesPerWord - 1, &pcm.flag_buf) ||
            pgl_malloc(flag_ct * sizeof(int32_t), &pcm.flag_map))) {
      goto main_ret_NOMEM_NOLOG2;
    }

    // No modifications to argv past this point.
    const char* const* argvk = TO_CONSTCPCONSTP(argv);

    char* flagname_write_iter = pcm.flag_buf;
    uint32_t cur_flag_idx = 0;
    for (arg_idx = first_arg_idx; arg_idx != S_CAST(uint32_t, argc); ++arg_idx) {
      flagname_p = IsCmdlineFlagStart(argvk[arg_idx]);
      if (flagname_p) {
        const uint32_t flag_slen = strlen(flagname_p);
        switch (*flagname_p) {
        case 'a':
          if (strequal_k(flagname_p, "aec", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "allow-extra-chr");
          } else if (strequal_k(flagname_p, "autosome-xy", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "autosome-par");
          } else if (strequal_k(flagname_p, "a1-allele", flag_slen)) {
            fputs("Warning: --a1-allele flag deprecated.  Use --alt1-allele instead.\n", stderr);
            g_stderr_written_to = 1;
            snprintf(flagname_write_iter, kMaxFlagBlen, "alt1-allele");
          } else if (strequal_k(flagname_p, "a2-allele", flag_slen)) {
            fputs("Warning: --a2-allele flag deprecated.  Use --ref-allele instead.\n", stderr);
            g_stderr_written_to = 1;
            snprintf(flagname_write_iter, kMaxFlagBlen, "ref-allele");
          } else {
            goto main_flag_copy;
          }
          break;
        case 'b':
          if (strequal_k(flagname_p, "bed", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "pgen");
          } else if (strequal_k(flagname_p, "bim", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "pvar");
          } else {
            goto main_flag_copy;
          }
          break;
        case 'e':
          if (strequal_k(flagname_p, "extract-if", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "keep-if-info");
          } else if (strequal_k(flagname_p, "exclude-if", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "remove-if-info");
          } else {
            goto main_flag_copy;
          }
          break;
        case 'f':
          if (strequal_k(flagname_p, "fam", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "psam");
          } else if (strequal_k(flagname_p, "filter-males", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "keep-males");
          } else if (strequal_k(flagname_p, "filter-females", flag_slen)) {
            fputs("Note: --filter-females flag deprecated.  Use --keep-females or --remove-males\ninstead.\n", stdout);
            snprintf(flagname_write_iter, kMaxFlagBlen, "remove-males");
          } else if (strequal_k(flagname_p, "filter-founders", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "keep-founders");
          } else if (strequal_k(flagname_p, "filter-nonfounders", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "keep-nonfounders");
          } else if (strequal_k(flagname_p, "filter", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "keep-fcol");
          } else {
            goto main_flag_copy;
          }
          break;
        case 'h':
          if (strequal_k(flagname_p, "hound", flag_slen)) {
            // the creature type should be Dog.
            snprintf(flagname_write_iter, kMaxFlagBlen, "dog");
          } else {
            goto main_flag_copy;
          }
          break;
        case 'k':
          if (strequal_k(flagname_p, "keep-clusters", flag_slen)) {
            fputs("Note: --keep-clusters flag deprecated.  Use --keep-cats instead.\n", stdout);
            snprintf(flagname_write_iter, kMaxFlagBlen, "keep-cats");
          } else if (strequal_k(flagname_p, "keep-cluster-names", flag_slen)) {
            fputs("Note: --keep-cluster-names flag deprecated.  Use --keep-cat-names instead.\n", stdout);
            snprintf(flagname_write_iter, kMaxFlagBlen, "keep-cat-names");
          } else if (strequal_k(flagname_p, "keep-if-info", flag_slen)) {
            fputs("Note: --keep-if-info renamed to --extract-if-info, for consistency with other\nsample/variant filters (keep/remove = sample filter; extract/exclude = variant\nfilter).\n", stdout);
            snprintf(flagname_write_iter, kMaxFlagBlen, "extract-if-info");
          } else {
            goto main_flag_copy;
          }
          break;
        case 'l':
          if (strequal_k(flagname_p, "linear", flag_slen) || strequal_k(flagname_p, "logistic", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "glm");
          } else {
            goto main_flag_copy;
          }
          break;
        case 'm':
          if (strequal_k(flagname_p, "min-ac", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "mac");
          } else if (strequal_k(flagname_p, "max-ac", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "max-mac");
          } else if (strequal_k(flagname_p, "min-af", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "maf");
          } else if (strequal_k(flagname_p, "max-af", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "max-maf");
          } else if (strequal_k(flagname_p, "make-bfile", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "make-bed");
          } else if (strequal_k(flagname_p, "make-bpfile", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "make-bpgen");
          } else if (strequal_k(flagname_p, "make-pfile", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "make-pgen");
          } else if (strequal_k(flagname_p, "missing_code", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "missing-code");
          } else if (strequal_k(flagname_p, "max-indv", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "thin-indiv-count");
          } else {
            goto main_flag_copy;
          }
          break;
        case 'n':
          if (strequal_k(flagname_p, "num_threads", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "threads");
          } else if (strequal_k(flagname_p, "no-pheno", flag_slen) ||
                     strequal_k(flagname_p, "no-fam-pheno", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "no-psam-pheno");
          } else if (strequal_k(flagname_p, "norm", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "normalize");
          } else {
            goto main_flag_copy;
          }
          break;
        case 'p':
          if (strequal_k(flagname_p, "prune", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "require-pheno");
          } else {
            goto main_flag_copy;
          }
          break;
        case 'q':
          if (strequal_k(flagname_p, "qual-scores", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "extract-fcol");
          } else if (strequal_k(flagname_p, "qual-threshold", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "extract-fcol-min");
          } else if (strequal_k(flagname_p, "qual-max-threshold", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "extract-fcol-max");
          } else {
            goto main_flag_copy;
          }
          break;
        case 'r':
          if (strequal_k(flagname_p, "recode", flag_slen)) {
            // special case: translate to "export ped" if no format specified
            const uint32_t param_ct = GetParamCt(argvk, argc, arg_idx);
            if (unlikely(param_ct > 4)) {
              fputs("Error: --recode accepts at most 4 parameters.\n", stderr);
              goto main_ret_INVALID_CMDLINE;
            }
            ExportfFlags dummy;
            IdpasteFlags dummy2;
            uint32_t format_param_idxs;
            GetExportfTargets(&(argvk[arg_idx]), param_ct, &dummy, &dummy2, &format_param_idxs);
            if (!format_param_idxs) {
              snprintf(flagname_write_iter, kMaxFlagBlen, "export ped");
            } else {
              snprintf(flagname_write_iter, kMaxFlagBlen, "export");
            }
          } else if (strequal_k(flagname_p, "remove-founders", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "keep-founders");
          } else if (strequal_k(flagname_p, "remove-nonfounders", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "keep-nonfounders");
          } else if (strequal_k(flagname_p, "remove-clusters", flag_slen)) {
            fputs("Note: --remove-clusters flag deprecated.  Use --remove-cats instead.\n", stdout);
            snprintf(flagname_write_iter, kMaxFlagBlen, "remove-cats");
          } else if (strequal_k(flagname_p, "remove-cluster-names", flag_slen)) {
            fputs("Note: --remove-cluster-names flag deprecated.  Use --remove-cat-names instead.\n", stdout);
            snprintf(flagname_write_iter, kMaxFlagBlen, "remove-cat-names");
          } else if (strequal_k(flagname_p, "remove-if-info", flag_slen)) {
            fputs("Note: --remove-if-info renamed to --exclude-if-info, for consistency with other\nsample/variant filters (keep/remove = sample filter; extract/exclude = variant\nfilter).\n", stdout);
            snprintf(flagname_write_iter, kMaxFlagBlen, "exclude-if-info");
          } else if (strequal_k(flagname_p, "recode-allele", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "export-allele");
          } else {
            goto main_flag_copy;
          }
          break;
        case 's':
          if (strequal_k(flagname_p, "sdiff", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "sample-diff");
          } else {
            goto main_flag_copy;
          }
          break;
        case 't':
          if (strequal_k(flagname_p, "thread-num", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "threads");
          } else {
            goto main_flag_copy;
          }
          break;
        case 'v':
          if (strequal_k(flagname_p, "vcf-filter", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "var-filter");
          } else if (strequal_k(flagname_p, "vcf-min-qual", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "var-min-qual");
          } else if (strequal_k(flagname_p, "vscore", flag_slen)) {
            snprintf(flagname_write_iter, kMaxFlagBlen, "variant-score");
          } else {
            goto main_flag_copy;
          }
          break;
        default:
        main_flag_copy:
          memcpy(flagname_write_iter, flagname_p, flag_slen + 1);
        }
        flagname_write_iter = &(flagname_write_iter[kMaxFlagBlen]);
        pcm.flag_map[cur_flag_idx++] = arg_idx;
      }
    }
    char outname[kPglFnamesize];
    memcpy_k(outname, "plink2", 6);
    char* outname_end = nullptr;
    char range_delim;
    int32_t known_procs;
    reterr = CmdlineParsePhase2(ver_str, errstr_append, argvk, 6, kMaxFlagBlen, argc, flag_ct, &pcm, outname, &outname_end, &range_delim, &known_procs, &pc.max_thread_ct);
    if (unlikely(reterr)) {
      goto main_ret_NOLOG;
    }

    char pgenname[kPglFnamesize];
    char psamname[kPglFnamesize];
    char pvarname[kPglFnamesize];
    pgenname[0] = '\0';
    psamname[0] = '\0';
    pvarname[0] = '\0';
    InitPheno();
    cur_flag_idx = 0;
    pc.command_flags1 = kfCommand10;
    // uint64_t command_flags2 = 0;
    pc.misc_flags = kfMisc0;
    pc.pvar_psam_flags = kfPvarPsam0;
    pc.sample_sort_flags = kfSort0;
    pc.sort_vars_flags = kfSort0;
    pc.grm_flags = kfGrm0;
    pc.pca_flags = kfPca0;
    pc.write_covar_flags = kfWriteCovar0;
    pc.pheno_transform_flags = kfPhenoTransform0;
    pc.fa_flags = kfFa0;
    pc.fam_cols = kfFamCol13456;
    pc.king_flags = kfKing0;
    pc.king_cutoff = -1;
    pc.king_table_filter = -DBL_MAX;
    pc.freq_rpt_flags = kfAlleleFreq0;
    pc.missing_rpt_flags = kfMissingRpt0;
    pc.geno_counts_flags = kfGenoCounts0;
    pc.hardy_flags = kfHardy0;
    pc.sample_counts_flags = kfSampleCounts0;
    pc.recover_var_ids_flags = kfRecoverVarIds0;
    pc.vscore_flags = kfVscore0;
    pc.rmdup_mode = kRmDup0;
    for (uint32_t uii = 0; uii != 4; ++uii) {
      pc.filter_modes[uii] = kFreqFilterNonmajor;
    }
    pc.aperm.min = 6;
    pc.aperm.max = 1000000;
    pc.aperm.alpha = 0.0;
    pc.aperm.beta = 0.0001;
    pc.aperm.init_interval = 1.0;
    pc.aperm.interval_slope = 0.001;
    pc.ci_size = 0.0;

    // Default value is 1638 = 32768 / 20, and that's applied to imported
    // dosages when --hard-call-threshold is not specified.
    // However, when --make-[b]pgen is run on a dosage-containing dataset,
    // explicit --hard-call-threshold will cause the hardcall set to be
    // regenerated, and that won't happen without --hard-call-threshold.  So we
    // need to distinguish between --hard-call-threshold 0.1 and no flag.
    pc.hard_call_thresh = UINT32_MAX;

    pc.dosage_erase_thresh = 0;
    pc.ln_pfilter = kLnPvalError;  // make --pfilter 1 still filter out NAs
    pc.output_min_ln = -DBL_MAX;
    pc.vif_thresh = 50.0;
    pc.mind_thresh = 1.0;
    pc.geno_thresh = 1.0;
    pc.hwe_thresh = 0.0;
    pc.mach_r2_min = 0.0;
    pc.mach_r2_max = 0.0;
    pc.minimac3_r2_min = 0.0;
    pc.minimac3_r2_max = 0.0;
    pc.min_maf = 0.0;
    pc.max_maf = 1.0;
    pc.thin_keep_prob = 1.0;
    pc.thin_keep_sample_prob = 1.0;
    pc.min_allele_dosage = 0;
    pc.max_allele_dosage = (~0LLU);
    pc.var_min_qual = -1;
    pc.update_sex_colm2 = 1;
    pc.new_variant_id_max_allele_slen = 23;
    pc.splitpar_bound1 = 0;
    pc.splitpar_bound2 = 0;
    pc.missing_pheno = -9;
    pc.from_bp = -1;
    pc.to_bp = -1;
    pc.window_bp = -1;
    pc.pca_ct = 0;
    pc.xchr_model = 2;
    pc.parallel_idx = 0;
    pc.parallel_tot = 1;
    pc.mwithin_val = 1;
    pc.min_bp_space = 0;
    pc.thin_keep_ct = UINT32_MAX;
    pc.thin_keep_sample_ct = UINT32_MAX;
    pc.keep_fcol_num = 0;
    pc.filter_min_allele_ct = 0;
    pc.filter_max_allele_ct = UINT32_MAX;
    double import_dosage_certainty = 0.0;
    int32_t vcf_min_gq = -1;
    int32_t vcf_min_dp = -1;
    int32_t vcf_max_dp = 0x7fffffff;
    intptr_t malloc_size_mib = 0;
    LoadParams load_params = kfLoadParams0;
    Xload xload = kfXload0;
    uint32_t rseed_ct = 0;
    MakePlink2Flags make_plink2_flags = kfMake0;
    OxfordImportFlags oxford_import_flags = kfOxfordImport0;
    VcfHalfCall vcf_half_call = kVcfHalfCallDefault;
    char id_delim = '\0';
    char idspace_to = '\0';
    char input_missing_geno_char = '0';
    char output_missing_geno_char = '.';
    ImportFlags import_flags = kfImport0;
    uint32_t aperm_present = 0;
    uint32_t notchr_present = 0;
    uint32_t permit_multiple_inclusion_filters = 0;
    uint32_t memory_require = 0;
    uint32_t randmem = 0;
    GenDummyInfo gendummy_info;
    InitGenDummy(&gendummy_info);
    Plink1DosageInfo plink1_dosage_info;
    InitPlink1Dosage(&plink1_dosage_info);
    do {
      flagname_p = &(pcm.flag_buf[cur_flag_idx * kMaxFlagBlen]);
      const char* flagname_p2 = &(flagname_p[1]);
      arg_idx = pcm.flag_map[cur_flag_idx];
      uint32_t param_ct = GetParamCt(argvk, argc, arg_idx);
      switch (*flagname_p) {
      case '1':
        if (likely(*flagname_p2 == '\0')) {
          pc.misc_flags |= kfMiscAffection01;
          goto main_param_zero;
        } else {
          goto main_ret_INVALID_CMDLINE_UNRECOGNIZED;
        }
        break;

      case 'a':
        if (strequal_k_unsafe(flagname_p2, "llow-extra-chr")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (param_ct) {
            const char* cur_modif = argvk[arg_idx + 1];
            if (unlikely(!strequal_k_unsafe(cur_modif, "0"))) {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --allow-extra-chr parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
            chr_info.zero_extra_chrs = 1;
          }
          pc.misc_flags |= kfMiscAllowExtraChrs;
        } else if (strequal_k_unsafe(flagname_p2, "utosome")) {
          pc.misc_flags |= kfMiscAutosomeOnly;
          chr_info.is_include_stack = 1;
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "utosome-par")) {
          if (unlikely(pc.misc_flags & kfMiscAutosomeOnly)) {
            logerrputs("Error: --autosome-par cannot be used with --autosome.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          pc.misc_flags |= kfMiscAutosomePar;
          chr_info.is_include_stack = 1;
          goto main_param_zero;
        } else if (unlikely(strequal_k_unsafe(flagname_p2, "llow-no-samples"))) {
          logerrputs("Error: --allow-no-samples is retired.  (If you are performing a set of\noperations which doesn't require sample information, the sample file won't be\nloaded at all.)\n");
          goto main_ret_INVALID_CMDLINE;
        } else if (unlikely(strequal_k_unsafe(flagname_p2, "llow-no-vars"))) {
          logerrputs("Error: --allow-no-vars is retired.  (If you are performing a set of operations\nwhich doesn't require variant information, the variant file won't be loaded at\nall.)\n");
          goto main_ret_INVALID_CMDLINE;
        } else if (strequal_k_unsafe(flagname_p2, "djust")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 4))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          for (uint32_t param_idx = 1; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "gc", cur_modif_slen)) {
              pc.adjust_info.flags |= kfAdjustGc;
            } else if (strequal_k(cur_modif, "log10", cur_modif_slen)) {
              pc.adjust_info.flags |= kfAdjustLog10;
            } else if (strequal_k(cur_modif, "zs", cur_modif_slen)) {
              pc.adjust_info.flags |= kfAdjustZs;
            } else if (unlikely(strequal_k(cur_modif, "qq-plot", cur_modif_slen))) {
              logerrputs("Error: 'qq-plot' modifier retired.  Use e.g. \"--adjust cols=+qq\" instead.\n");
              goto main_ret_INVALID_CMDLINE_A;
            } else if (likely(StrStartsWith(cur_modif, "cols=", cur_modif_slen))) {
              if (unlikely(pc.adjust_info.flags & kfAdjustColAll)) {
                logerrputs("Error: Multiple --adjust cols= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              reterr = ParseColDescriptor(&(cur_modif[5]), "chrom\0pos\0ref\0alt1\0alt\0a1\tunadj\0gc\0qq\0bonf\0holm\0sidakss\0sidaksd\0fdrbh\0fdrby\0", "adjust", kfAdjustColChrom, kfAdjustColDefault, 1, &pc.adjust_info.flags);
              if (unlikely(reterr)) {
                goto main_ret_1;
              }
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --adjust parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          if (!(pc.adjust_info.flags & kfAdjustColAll)) {
            pc.adjust_info.flags |= kfAdjustColDefault;
          }
        } else if (strequal_k_unsafe(flagname_p2, "djust-file")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 7))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocFname(argvk[arg_idx + 1], flagname_p, 0, &adjust_file_info.fname);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          for (uint32_t param_idx = 2; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "gc", cur_modif_slen)) {
              adjust_file_info.base.flags |= kfAdjustGc;
            } else if (strequal_k(cur_modif, "log10", cur_modif_slen)) {
              adjust_file_info.base.flags |= kfAdjustLog10;
            } else if (strequal_k(cur_modif, "zs", cur_modif_slen)) {
              adjust_file_info.base.flags |= kfAdjustZs;
            } else if (StrStartsWith(cur_modif, "cols=", cur_modif_slen)) {
              if (unlikely(adjust_file_info.base.flags & kfAdjustColAll)) {
                logerrputs("Error: Multiple --adjust-file cols= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              reterr = ParseColDescriptor(&(cur_modif[5]), "chrom\0pos\0ref\0alt1\0alt\0a1\0unadj\0gc\0qq\0bonf\0holm\0sidakss\0sidaksd\0fdrbh\0fdrby\0", "adjust-file", kfAdjustColChrom, kfAdjustColDefault, 1, &adjust_file_info.base.flags);
              if (unlikely(reterr)) {
                goto main_ret_1;
              }
            } else if (StrStartsWith(cur_modif, "test=", cur_modif_slen)) {
              reterr = CmdlineAllocString(&(cur_modif[5]), "--adjust-file test=", kMaxIdSlen, &adjust_file_info.test_name);
              if (unlikely(reterr)) {
                goto main_ret_1;
              }
            } else if (likely(strequal_k(cur_modif, "input-log10", cur_modif_slen))) {
              adjust_file_info.base.flags |= kfAdjustInputLog10;
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --adjust-file parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          if (!(adjust_file_info.base.flags & kfAdjustColAll)) {
            adjust_file_info.base.flags |= kfAdjustColDefault;
          }
        } else if (strequal_k_unsafe(flagname_p2, "djust-chr-field")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 0x7fffffff))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocAndFlatten(&(argvk[arg_idx + 1]), param_ct, 0x7fffffff, &adjust_file_info.chr_field);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
        } else if (strequal_k_unsafe(flagname_p2, "djust-alt-field")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 0x7fffffff))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocAndFlatten(&(argvk[arg_idx + 1]), param_ct, 0x7fffffff, &adjust_file_info.alt_field);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
        } else if (strequal_k_unsafe(flagname_p2, "djust-a1-field")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 0x7fffffff))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocAndFlatten(&(argvk[arg_idx + 1]), param_ct, 0x7fffffff, &adjust_file_info.a1_field);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
        } else if (strequal_k_unsafe(flagname_p2, "djust-pos-field")) {
          if (unlikely(!adjust_file_info.fname)) {
            logerrputs("Error: --adjust-pos-field must be used with --adjust-file.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 0x7fffffff))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocAndFlatten(&(argvk[arg_idx + 1]), param_ct, 0x7fffffff, &adjust_file_info.pos_field);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
        } else if (strequal_k_unsafe(flagname_p2, "djust-id-field")) {
          if (unlikely(!adjust_file_info.fname)) {
            logerrputs("Error: --adjust-id-field must be used with --adjust-file.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 0x7fffffff))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocAndFlatten(&(argvk[arg_idx + 1]), param_ct, 0x7fffffff, &adjust_file_info.id_field);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
        } else if (strequal_k_unsafe(flagname_p2, "djust-ref-field")) {
          if (unlikely(!adjust_file_info.fname)) {
            logerrputs("Error: --adjust-ref-field must be used with --adjust-file.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 0x7fffffff))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocAndFlatten(&(argvk[arg_idx + 1]), param_ct, 0x7fffffff, &adjust_file_info.ref_field);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
        } else if (strequal_k_unsafe(flagname_p2, "djust-test-field")) {
          if (unlikely(!adjust_file_info.fname)) {
            logerrputs("Error: --adjust-test-field must be used with --adjust-file.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 0x7fffffff))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocAndFlatten(&(argvk[arg_idx + 1]), param_ct, 0x7fffffff, &adjust_file_info.test_field);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
        } else if (strequal_k_unsafe(flagname_p2, "djust-p-field")) {
          if (unlikely(!adjust_file_info.fname)) {
            logerrputs("Error: --adjust-p-field must be used with --adjust-file.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 0x7fffffff))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocAndFlatten(&(argvk[arg_idx + 1]), param_ct, 0x7fffffff, &adjust_file_info.p_field);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
        } else if (strequal_k_unsafe(flagname_p2, "perm")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 6))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          if (unlikely(ScanPosintDefcapx(cur_modif, &pc.aperm.min))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --aperm min permutation count '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          ++pc.aperm.min;
          if (param_ct > 1) {
            cur_modif = argvk[arg_idx + 2];
            if (unlikely(ScanPosintCappedx(cur_modif, kApermMax, &pc.aperm.max))) {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --aperm max permutation count '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          if (unlikely(pc.aperm.min >= pc.aperm.max)) {
            logerrputs("Error: --aperm min permutation count must be smaller than max.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          aperm_present = 1;
          if (param_ct > 2) {
            cur_modif = argvk[arg_idx + 3];
            if (unlikely(!ScantokDouble(cur_modif, &pc.aperm.alpha))) {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --aperm alpha threshold '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
            if (param_ct > 3) {
              cur_modif = argvk[arg_idx + 4];
              if (unlikely((!ScantokDouble(cur_modif, &pc.aperm.beta)) || (pc.aperm.beta <= 0.0))) {
                snprintf(g_logbuf, kLogbufSize, "Error: Invalid --aperm beta '%s'.\n", cur_modif);
                goto main_ret_INVALID_CMDLINE_WWA;
              }
              if (param_ct > 4) {
                cur_modif = argvk[arg_idx + 5];
                if (unlikely(!ScantokDouble(cur_modif, &pc.aperm.init_interval))) {
                  snprintf(g_logbuf, kLogbufSize, "Error: Invalid --aperm initial pruning interval '%s'.\n", cur_modif);
                  goto main_ret_INVALID_CMDLINE_WWA;
                }
                if (unlikely((pc.aperm.init_interval < 1.0) || (pc.aperm.init_interval > 1000000.0))) {
                  snprintf(g_logbuf, kLogbufSize, "Error: Invalid --aperm initial pruning interval '%s'.\n", cur_modif);
                  goto main_ret_INVALID_CMDLINE_WWA;
                }
                if (param_ct == 6) {
                  cur_modif = argvk[arg_idx + 6];
                  if (unlikely((!ScantokDouble(cur_modif, &pc.aperm.interval_slope)) || (pc.aperm.interval_slope < 0.0) || (pc.aperm.interval_slope > 1.0))) {
                    snprintf(g_logbuf, kLogbufSize, "Error: Invalid --aperm pruning interval slope '%s'.\n", cur_modif);
                    goto main_ret_INVALID_CMDLINE_WWA;
                  }
                }
              }
            }
          }
        } else if (strequal_k_unsafe(flagname_p2, "utosome-num")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          uint32_t autosome_ct;
          if (unlikely(ScanPosintCappedx(cur_modif, kMaxChrTextnum, &autosome_ct))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --autosome-num parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          // see plink2_common FinalizeChrset()
          chr_info.chrset_source = kChrsetSourceCmdline;
          chr_info.autosome_ct = autosome_ct;
          // assumes first code is X
          chr_info.xymt_codes[0] = autosome_ct + 1;
          for (uint32_t xymt_idx = 1; xymt_idx != kChrOffsetCt; ++xymt_idx) {
            // bugfix: this needs to be UINT32_MAXM1, not UINT32_MAX, for
            // GetChrCode() to work properly
            chr_info.xymt_codes[xymt_idx] = UINT32_MAXM1;
          }
          chr_info.haploid_mask[0] = 0;
          SetBit(autosome_ct + 1, chr_info.haploid_mask);
        } else if (strequal_k_unsafe(flagname_p2, "lt1-allele")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 5))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* const* sources = &(argvk[arg_idx + 1]);
          if (!strcmp(sources[0], "force")) {
            --param_ct;
            if (unlikely(!param_ct)) {
              logerrputs("Error: Invalid --alt1-allele parameter sequence.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
            pc.misc_flags |= kfMiscAlt1AlleleForce;
            ++sources;
          }
          reterr = Alloc2col(sources, flagname_p, param_ct, &pc.alt1_allele_flag);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.dependency_flags |= kfFilterPvarReq;
        } else if (unlikely(strequal_k_unsafe(flagname_p2, "ssoc"))) {
          logerrputs("Error: --assoc is retired.  Use --glm instead.\n");
          goto main_ret_INVALID_CMDLINE_A;
        } else if (likely(strequal_k_unsafe(flagname_p2, "llow-no-sex"))) {
          logputs("Note: --allow-no-sex no longer has any effect.  (Missing-sex samples are\nautomatically excluded from association analysis when sex is a covariate, and\ntreated normally otherwise.)\n");
          goto main_param_zero;
        } else {
          goto main_ret_INVALID_CMDLINE_UNRECOGNIZED;
        }
        break;

      case 'b':
        if (strequal_k_unsafe(flagname_p2, "file")) {
          if (unlikely(xload)) {
            goto main_ret_INVALID_CMDLINE_INPUT_CONFLICT;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          uint32_t fname_modif_idx = 1;
          if (param_ct == 2) {
            if (unlikely(CheckExtraParam(&(argvk[arg_idx]), "vzs", &fname_modif_idx))) {
              goto main_ret_INVALID_CMDLINE_A;
            }
          }
          const char* fname_prefix = argvk[arg_idx + fname_modif_idx];
          const uint32_t slen = strlen(fname_prefix);
          // pathological case bugfix (1 Feb 2018): need to subtract 1 more, to
          // avoid buffer overflow in the case we rename and append '~'.
          if (unlikely(slen > (kPglFnamesize - 10))) {
            // could use kPglFnamesize - 2 - 3 * param_ct, but that's pointless
            logerrputs("Error: --bfile parameter too long.\n");
            goto main_ret_OPEN_FAIL;
          }
          snprintf(memcpya(pgenname, fname_prefix, slen), 9, ".bed");
          snprintf(memcpya(psamname, fname_prefix, slen), 9, ".fam");
          char* bimname_end = memcpya(pvarname, fname_prefix, slen);
          bimname_end = Stpcpy(bimname_end, ".bim");
          if (param_ct == 2) {
            snprintf(bimname_end, 5, ".zst");
          }
          load_params |= kfLoadParamsPfileAll;
        } else if (strequal_k_unsafe(flagname_p2, "pfile")) {
          if (unlikely(load_params || xload)) {
            // currently only possible with --bcf, --bfile
            goto main_ret_INVALID_CMDLINE_INPUT_CONFLICT;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          uint32_t fname_modif_idx = 1;
          if (param_ct == 2) {
            if (unlikely(CheckExtraParam(&(argvk[arg_idx]), "vzs", &fname_modif_idx))) {
              goto main_ret_INVALID_CMDLINE_A;
            }
          }
          const char* fname_prefix = argvk[arg_idx + fname_modif_idx];
          const uint32_t slen = strlen(fname_prefix);
          if (unlikely(slen > (kPglFnamesize - 10))) {
            logerrputs("Error: --bpfile parameter too long.\n");
            goto main_ret_OPEN_FAIL;
          }
          snprintf(memcpya(pgenname, fname_prefix, slen), 9, ".pgen");
          snprintf(memcpya(psamname, fname_prefix, slen), 9, ".fam");
          char* bimname_end = memcpya(pvarname, fname_prefix, slen);
          bimname_end = Stpcpy(bimname_end, ".bim");
          if (param_ct == 2) {
            snprintf(bimname_end, 5, ".zst");
          }
          load_params |= kfLoadParamsPfileAll;
        } else if (strequal_k_unsafe(flagname_p2, "iallelic-only")) {
          logerrputs("Error: --biallelic-only is retired.  Use e.g. \"--max-alleles 2\" instead.\n");
          goto main_ret_INVALID_CMDLINE_A;
        } else if (strequal_k_unsafe(flagname_p2, "cf")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (param_ct == 2) {
            const char* cur_modif = argvk[arg_idx + 2];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (unlikely(!StrStartsWith(cur_modif, "dosage=", cur_modif_slen))) {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --bcf parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
            reterr = CmdlineAllocString(&(cur_modif[strlen("dosage=")]), argvk[arg_idx], 4095, &vcf_dosage_import_field);
            if (unlikely(reterr)) {
              goto main_ret_1;
            }
            if (unlikely(!((!strcmp(vcf_dosage_import_field, "GP-force")) || IsAlphanumeric(vcf_dosage_import_field)))) {
              logerrputs("Error: --bcf dosage= parameter is not alphanumeric.\n");
              goto main_ret_INVALID_CMDLINE;
            }
            if (unlikely(!strcmp(vcf_dosage_import_field, "GT"))) {
              logerrputs("Error: --bcf dosage= parameter cannot be 'GT'.\n");
              goto main_ret_INVALID_CMDLINE;
            }
          }
          const char* cur_modif = argvk[arg_idx + 1];
          const uint32_t slen = strlen(cur_modif);
          if (unlikely(slen > kPglFnamesize - 1)) {
            logerrputs("Error: --bcf filename too long.\n");
            goto main_ret_OPEN_FAIL;
          }
          memcpy(pgenname, cur_modif, slen + 1);
          xload = kfXloadBcf;
        } else if (strequal_k_unsafe(flagname_p2, "gen")) {
          if (unlikely(load_params || xload)) {
            goto main_ret_INVALID_CMDLINE_INPUT_CONFLICT;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 3))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          for (uint32_t param_idx = 2; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "snpid-chr", cur_modif_slen)) {
              oxford_import_flags |= kfOxfordImportBgenSnpIdChr;
            } else if (strequal_k(cur_modif, "ref-first", cur_modif_slen)) {
              oxford_import_flags |= kfOxfordImportRefFirst;
            } else if (strequal_k(cur_modif, "ref-unknown", cur_modif_slen)) {
              oxford_import_flags |= kfOxfordImportRefUnknown;
            } else if (strequal_k(cur_modif, "ref-last", cur_modif_slen)) {
              oxford_import_flags |= kfOxfordImportRefLast;
            } else if (likely(strequal_k(cur_modif, "ref-second", cur_modif_slen))) {
              logerrputs("Warning: --bgen 'ref-second' modifier is deprecated.  Use 'ref-last' instead.\n");
              oxford_import_flags |= kfOxfordImportRefLast;
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --bgen parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          if (!(oxford_import_flags & kfOxfordImportRefAll)) {
            logerrputs("Warning: No --bgen REF/ALT mode specified ('ref-first', 'ref-last', or\n'ref-unknown').  This will be required as of alpha 3.\n");
          }
          const char* cur_fname = argvk[arg_idx + 1];
          const uint32_t slen = strlen(cur_fname);
          if (unlikely(slen > kPglFnamesize - 1)) {
            logerrputs("Error: --bgen filename too long.\n");
            goto main_ret_OPEN_FAIL;
          }
          memcpy(pgenname, cur_fname, slen + 1);
          xload = kfXloadOxBgen;
        } else if (strequal_k_unsafe(flagname_p2, "p-space")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          if (unlikely(ScanPosintDefcapx(cur_modif, &pc.min_bp_space))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --bp-space minimum bp distance '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          pc.filter_flags |= kfFilterPvarReq | kfFilterNoSplitChr;
        } else if (likely(strequal_k_unsafe(flagname_p2, "ad-freqs"))) {
          pc.misc_flags |= kfMiscAllowBadFreqs;
          goto main_param_zero;
        } else {
          goto main_ret_INVALID_CMDLINE_UNRECOGNIZED;
        }
        break;

      case 'c':
        if (strequal_k_unsafe(flagname_p2, "hr")) {
          if (unlikely(pc.misc_flags & (kfMiscAutosomePar | kfMiscAutosomeOnly))) {
            logerrputs("Error: --chr cannot be used with --autosome{-par}.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 0x7fffffff))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = ParseChrRanges(&(argvk[arg_idx]), flagname_p, errstr_append, param_ct, (pc.misc_flags / kfMiscAllowExtraChrs) & 1, 0, '-', &chr_info, chr_info.chr_mask);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          chr_info.is_include_stack = 1;
        } else if (strequal_k_unsafe(flagname_p2, "ovar")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          uint32_t fname_idx = 1;
          if (param_ct == 2) {
            if (unlikely(CheckExtraParam(&(argvk[arg_idx]), "iid-only", &fname_idx))) {
              goto main_ret_INVALID_CMDLINE_A;
            }
            pc.misc_flags |= kfMiscCovarIidOnly;
          }
          reterr = AllocFname(argvk[arg_idx + fname_idx], flagname_p, 0, &pc.covar_fname);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.dependency_flags |= kfFilterPsamReq;
        } else if (strequal_k_unsafe(flagname_p2, "ovar-col-nums")) {
          // requires --covar or --pheno, but --pheno hasn't been parsed yet so
          // we don't enforce the condition here
          reterr = ParseNameRanges(&(argvk[arg_idx]), errstr_append, param_ct, 1, '-', &pc.covar_range_list);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.misc_flags |= kfMiscCovarColNums;
        } else if (strequal_k_unsafe(flagname_p2, "ovar-name")) {
          if (unlikely(pc.covar_range_list.name_ct)) {
            logerrputs("Error: --covar-name can't be used with --covar-col-nums.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          // can now be used without --covar
          reterr = ParseNameRanges(&(argvk[arg_idx]), errstr_append, param_ct, 0, range_delim, &pc.covar_range_list);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.dependency_flags |= kfFilterPsamReq;
        } else if (strequal_k_unsafe(flagname_p2, "onst-fid")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = CmdlineAllocString(param_ct? argvk[arg_idx + 1] : "0", argvk[arg_idx], kMaxIdSlen, &const_fid);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
        } else if (strequal_k_unsafe(flagname_p2, "i")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (unlikely(!ScantokDouble(argvk[arg_idx + 1], &pc.ci_size))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --ci parameter '%s'.\n", argvk[arg_idx + 1]);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          if (unlikely((pc.ci_size < 0.01) || (pc.ci_size >= 1.0))) {
            logerrputs("Error: --ci confidence interval size must be in [0.01, 1).\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
        } else if (strequal_k_unsafe(flagname_p2, "ondition")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 3))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = CmdlineAllocString(argvk[arg_idx + 1], argvk[arg_idx], kMaxIdSlen, &pc.glm_info.condition_varname);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          for (uint32_t param_idx = 2; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "dominant", cur_modif_slen)) {
              pc.glm_info.flags |= kfGlmConditionDominant;
            } else if (strequal_k(cur_modif, "recessive", cur_modif_slen)) {
              pc.glm_info.flags |= kfGlmConditionRecessive;
            } else if (likely(
                strequal_k(cur_modif, "multiallelic", cur_modif_slen) ||
                strequal_k(cur_modif, "m", cur_modif_slen))) {
              pc.glm_info.flags |= kfGlmConditionMultiallelic;
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --condition parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          if (unlikely((pc.glm_info.flags & (kfGlmConditionDominant | kfGlmConditionRecessive)) == (kfGlmConditionDominant | kfGlmConditionRecessive))) {
            logerrputs("Error: --condition 'dominant' and 'recessive' modifiers can't be used together.\n");
            goto main_ret_INVALID_CMDLINE;
          }
        } else if (strequal_k_unsafe(flagname_p2, "ondition-list")) {
          if (unlikely(pc.glm_info.condition_varname)) {
            logerrputs("Error: --condition-list cannot be used with --condition.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 3))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocFname(argvk[arg_idx + 1], flagname_p, 0, &pc.glm_info.condition_list_fname);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          for (uint32_t param_idx = 2; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "dominant", cur_modif_slen)) {
              pc.glm_info.flags |= kfGlmConditionDominant;
            } else if (strequal_k(cur_modif, "recessive", cur_modif_slen)) {
              pc.glm_info.flags |= kfGlmConditionRecessive;
            } else if (likely(
                strequal_k(cur_modif, "multiallelic", cur_modif_slen) ||
                strequal_k(cur_modif, "m", cur_modif_slen))) {
              pc.glm_info.flags |= kfGlmConditionMultiallelic;
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --condition-list parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          if (unlikely((pc.glm_info.flags & (kfGlmConditionDominant | kfGlmConditionRecessive)) == (kfGlmConditionDominant | kfGlmConditionRecessive))) {
            logerrputs("Error: --condition-list 'dominant' and 'recessive' modifiers can't be used\ntogether.\n");
            goto main_ret_INVALID_CMDLINE;
          }
        } else if (strequal_k_unsafe(flagname_p2, "ow")) {
          if (unlikely(chr_info.chrset_source)) {
            logerrputs("Error: Conflicting chromosome-set flags.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          chr_info.chrset_source = kChrsetSourceCmdline;
          // initialize here instead of FinalizeChrset(), to simplify
          // ReadChrsetHeaderLine()
          chr_info.autosome_ct = 29;
          chr_info.xymt_codes[0] = 30;
          chr_info.xymt_codes[1] = 31;
          chr_info.xymt_codes[2] = UINT32_MAXM1;
          chr_info.xymt_codes[3] = 33;
          chr_info.xymt_codes[4] = UINT32_MAXM1;
          chr_info.xymt_codes[5] = UINT32_MAXM1;
#ifdef __LP64__
          chr_info.haploid_mask[0] = 0x2c0000000LLU;
#else
          chr_info.haploid_mask[0] = 0xc0000000U;
          chr_info.haploid_mask[1] = 2;
#endif
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "hr-set")) {
          if (unlikely(chr_info.chrset_source)) {
            logerrputs("Error: Conflicting chromosome-set flags.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 5))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          int32_t signed_autosome_ct;
          if (unlikely(ScanIntAbsBoundedx(cur_modif, kMaxChrTextnum, &signed_autosome_ct) || (!signed_autosome_ct))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --chr-set parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          // see plink2_common FinalizeChrset()
          chr_info.chrset_source = kChrsetSourceCmdline;
          chr_info.haploid_mask[0] = 0;
          if (signed_autosome_ct < 0) {
            // haploid
            if (unlikely(param_ct > 1)) {
              logerrputs("Error: --chr-set does not accept multiple parameters in haploid mode.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
            const uint32_t autosome_ct = -signed_autosome_ct;
            chr_info.autosome_ct = autosome_ct;
            for (uint32_t xymt_idx = 0; xymt_idx != kChrOffsetCt; ++xymt_idx) {
              chr_info.xymt_codes[xymt_idx] = UINT32_MAXM1;
            }
            SetAllBits(autosome_ct + 1, chr_info.haploid_mask);
          } else {
            const uint32_t autosome_ct = signed_autosome_ct;
            chr_info.autosome_ct = autosome_ct;
            // assumes first four codes are x, y, xy, mt
            for (uint32_t xymt_idx = 0; xymt_idx != 4; ++xymt_idx) {
              chr_info.xymt_codes[xymt_idx] = autosome_ct + 1 + xymt_idx;
            }
            for (uint32_t xymt_idx = 4; xymt_idx != kChrOffsetCt; ++xymt_idx) {
              chr_info.xymt_codes[xymt_idx] = UINT32_MAXM1;
            }
            SetBit(autosome_ct + 1, chr_info.haploid_mask);
            SetBit(autosome_ct + 2, chr_info.haploid_mask);
            for (uint32_t param_idx = 2; param_idx <= param_ct; ++param_idx) {
              cur_modif = argvk[arg_idx + param_idx];
              const uint32_t cur_modif_slen = strlen(cur_modif);
              if (strequal_k(cur_modif, "no-x", cur_modif_slen)) {
                chr_info.xymt_codes[0] = UINT32_MAXM1;
                ClearBit(autosome_ct + 1, chr_info.haploid_mask);
              } else if (strequal_k(cur_modif, "no-y", cur_modif_slen)) {
                chr_info.xymt_codes[1] = UINT32_MAXM1;
                ClearBit(autosome_ct + 2, chr_info.haploid_mask);
              } else if (strequal_k(cur_modif, "no-xy", cur_modif_slen)) {
                chr_info.xymt_codes[2] = UINT32_MAXM1;
              } else if (likely(strequal_k(cur_modif, "no-mt", cur_modif_slen))) {
                chr_info.xymt_codes[3] = UINT32_MAXM1;
                ClearBit(autosome_ct + 4, chr_info.haploid_mask);
              } else {
                snprintf(g_logbuf, kLogbufSize, "Error: Invalid --chr-set parameter '%s'.\n", cur_modif);
                goto main_ret_INVALID_CMDLINE_WWA;
              }
            }
          }
        } else if (strequal_k_unsafe(flagname_p2, "hr-override")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 1))) {
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          if (param_ct) {
            const char* cur_modif = argvk[arg_idx + 1];
            if (likely(!strcmp(cur_modif, "file"))) {
              pc.misc_flags |= kfMiscChrOverrideFile;
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --chr-override parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          } else {
            pc.misc_flags |= kfMiscChrOverrideCmdline;
          }
        } else if (strequal_k_unsafe(flagname_p2, "ovar-quantile-normalize")) {
          if (param_ct) {
            reterr = AllocAndFlatten(&(argvk[arg_idx + 1]), param_ct, 0x7fffffff, &pc.covar_quantnorm_flattened);
            if (unlikely(reterr)) {
              goto main_ret_1;
            }
          }
          pc.pheno_transform_flags |= kfPhenoTransformQuantnormCovar;
          pc.dependency_flags |= kfFilterPsamReq;
        } else if (strequal_k_unsafe(flagname_p2, "ovar-number")) {
          logerrputs("Error: --covar-number is retired.  Use --covar-col-nums instead (and add 2 to\nconvert from PLINK 1.x covariate-indexes to covariate-column-numbers).\n");
          goto main_ret_INVALID_CMDLINE_A;
        } else if (likely(strequal_k_unsafe(flagname_p2, "ovar-variance-standardize"))) {
          if (param_ct) {
            reterr = AllocAndFlatten(&(argvk[arg_idx + 1]), param_ct, 0x7fffffff, &pc.vstd_flattened);
            if (unlikely(reterr)) {
              goto main_ret_1;
            }
          }
          pc.pheno_transform_flags |= kfPhenoTransformVstdCovar;
          pc.dependency_flags |= kfFilterPsamReq;
        } else {
          goto main_ret_INVALID_CMDLINE_UNRECOGNIZED;
        }
        break;

      case 'd':
        if (strequal_k_unsafe(flagname_p2, "ouble-id")) {
          if (unlikely(const_fid)) {
            logerrputs("Error: --double-id cannot be used with --const-fid.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          import_flags |= kfImportDoubleId;
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "ebug")) {
          g_debug_on = 1;
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "ata")) {
          if (unlikely(load_params || (xload & (~kfXloadOxBgen)))) {
            goto main_ret_INVALID_CMDLINE_INPUT_CONFLICT;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 3))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          uint32_t is_gzs = 0;
          for (uint32_t param_idx = 2; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "ref-first", cur_modif_slen)) {
              oxford_import_flags |= kfOxfordImportRefFirst;
            } else if (strequal_k(cur_modif, "ref-unknown", cur_modif_slen)) {
              oxford_import_flags |= kfOxfordImportRefUnknown;
            } else if (strequal_k(cur_modif, "ref-last", cur_modif_slen) ||
                       strequal_k(cur_modif, "ref-second", cur_modif_slen)) {
              oxford_import_flags |= kfOxfordImportRefLast;
            } else if (likely(strequal_k(cur_modif, "gzs", cur_modif_slen))) {
              if (unlikely(xload & kfXloadOxBgen)) {
                // may as well permit e.g. --data ref-first + --bgen
                logerrputs("Error: --data 'gzs' modifier cannot be used with .bgen input.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              is_gzs = 1;
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --data parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          if (!(oxford_import_flags & kfOxfordImportRefAll)) {
            logerrputs("Warning: No --data REF/ALT mode specified ('ref-first', 'ref-last', or\n'ref-unknown').  This will be required as of alpha 3.\n");
          }
          const char* fname_prefix = argvk[arg_idx + 1];
          const uint32_t slen = strlen(fname_prefix);
          if (unlikely(slen > (kPglFnamesize - 9))) {
            logerrputs("Error: --data parameter too long.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          if (!(xload & kfXloadOxBgen)) {
            // allow --bgen to override this
            char* genname_end = memcpya(pgenname, fname_prefix, slen);
            genname_end = Stpcpy(genname_end, ".gen");
            if (is_gzs) {
              snprintf(genname_end, 5, ".zst");
            }
            xload |= kfXloadOxGen;
          }
          snprintf(memcpya(psamname, fname_prefix, slen), 9, ".sample");
          xload |= kfXloadOxSample;
        } else if (strequal_k_unsafe(flagname_p2, "osage-erase-threshold")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          double dosage_erase_frac;
          if (unlikely((!ScantokDouble(cur_modif, &dosage_erase_frac)) || (dosage_erase_frac < 0.0) || (dosage_erase_frac >= (0.5 - kSmallEpsilon)))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --dosage-erase-threshold parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          pc.dosage_erase_thresh = S_CAST(int32_t, dosage_erase_frac * ((1 + kSmallEpsilon) * kDosageMid));
        } else if (strequal_k_unsafe(flagname_p2, "ummy")) {
          if (unlikely(load_params || xload)) {
            goto main_ret_INVALID_CMDLINE_INPUT_CONFLICT;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 2, 8))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (unlikely(ScanPosintDefcapx(argvk[arg_idx + 1], &gendummy_info.sample_ct))) {
            logerrputs("Error: Invalid --dummy sample count.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(ScanPosintDefcapx(argvk[arg_idx + 2], &gendummy_info.variant_ct))) {
            logerrputs("Error: Invalid --dummy SNP count.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          uint32_t extra_numeric_param_ct = 0;
          for (uint32_t param_idx = 3; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (MatchUpperKLen(cur_modif, "ACGT", cur_modif_slen)) {
              gendummy_info.flags |= kfGenDummyAcgt;
            } else if (strequal_k(cur_modif, "1234", cur_modif_slen)) {
              gendummy_info.flags |= kfGenDummy1234;
            } else if (strequal_k(cur_modif, "12", cur_modif_slen)) {
              gendummy_info.flags |= kfGenDummy12;
            } else if (StrStartsWith(cur_modif, "pheno-ct=", cur_modif_slen)) {
              const char* pheno_ct_start = &(cur_modif[strlen("pheno-ct=")]);
              if (unlikely(ScanUintCappedx(pheno_ct_start, kMaxPhenoCt, &gendummy_info.pheno_ct))) {
                snprintf(g_logbuf, kLogbufSize, "Error: Invalid --dummy pheno-ct= parameter '%s'.\n", pheno_ct_start);
                goto main_ret_INVALID_CMDLINE_WWA;
              }
            } else if (strequal_k(cur_modif, "scalar-pheno", cur_modif_slen)) {
              gendummy_info.flags |= kfGenDummyScalarPheno;
            } else if (StrStartsWith(cur_modif, "dosage-freq=", cur_modif_slen)) {
              const char* dosage_freq_start = &(cur_modif[strlen("dosage-freq=")]);
              double dxx;
              if (unlikely((!ScantokDouble(dosage_freq_start, &dxx)) || (dxx < 0.0) || (dxx > 1.0))) {
                snprintf(g_logbuf, kLogbufSize, "Error: Invalid --dummy dosage-freq= parameter '%s'.\n", dosage_freq_start);
                goto main_ret_INVALID_CMDLINE_WWA;
              }
              gendummy_info.dosage_freq = dxx;
            } else {
              double dxx;
              if (unlikely((extra_numeric_param_ct == 2) || (!ScantokDouble(cur_modif, &dxx)) || (dxx < 0.0) || (dxx > 1.0))) {
                snprintf(g_logbuf, kLogbufSize, "Error: Invalid --dummy parameter '%s'.\n", cur_modif);
                goto main_ret_INVALID_CMDLINE_WWA;
              }
              if (!extra_numeric_param_ct) {
                gendummy_info.geno_mfreq = dxx;
              } else {
                gendummy_info.pheno_mfreq = dxx;
              }
              ++extra_numeric_param_ct;
            }
          }
          const uint32_t mutually_exclusive_flags = gendummy_info.flags & (kfGenDummyAcgt | kfGenDummy1234 | kfGenDummy12);
          if (unlikely(mutually_exclusive_flags & (mutually_exclusive_flags - 1))) {
            logerrputs("Error: --dummy 'acgt', '1234', and '12' modifiers are mutually exclusive.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          xload |= kfXloadGenDummy;
        } else if (unlikely(strequal_k_unsafe(flagname_p2, "ummy-coding"))) {
          logerrputs("Error: --dummy-coding is retired.  Use --split-cat-pheno instead.\n");
          goto main_ret_INVALID_CMDLINE_A;
        } else if (unlikely(strequal_k_unsafe(flagname_p2, "osage"))) {
          logerrputs("Error: --dosage has been replaced with --import-dosage, which converts to .pgen\nformat and provides access to the full range of plink2 flags.  (Run --glm on\nthe imported dataset to invoke the original --dosage linear/logistic\nregression.)\n");
          goto main_ret_INVALID_CMDLINE_A;
        } else if (strequal_k_unsafe(flagname_p2, "og")) {
          if (unlikely(chr_info.chrset_source)) {
            logerrputs("Error: Conflicting chromosome-set flags.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          chr_info.chrset_source = kChrsetSourceCmdline;
          chr_info.autosome_ct = 38;
          chr_info.xymt_codes[0] = 39;
          chr_info.xymt_codes[1] = 40;
          chr_info.xymt_codes[2] = 41;
          chr_info.xymt_codes[3] = 42;
          chr_info.xymt_codes[4] = UINT32_MAXM1;
          chr_info.xymt_codes[5] = UINT32_MAXM1;
#ifdef __LP64__
          chr_info.haploid_mask[0] = 0x58000000000LLU;
#else
          chr_info.haploid_mask[0] = 0;
          chr_info.haploid_mask[1] = 0x580;
#endif
          goto main_param_zero;
        } else if (unlikely(*flagname_p2)) {
          // --d is preprocessed
          goto main_ret_INVALID_CMDLINE_UNRECOGNIZED;
        }
        break;

      case 'e':
        if (strequal_k_unsafe(flagname_p2, "xtract")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 0x7fffffff))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          const uint32_t cur_modif_slen = strlen(cur_modif);
          uint32_t is_interval_bed = 0;
          if (strequal_k(cur_modif, "bed0", cur_modif_slen) || strequal_k(cur_modif, "ibed0", cur_modif_slen)) {
            if (unlikely(param_ct == 1)) {
              logerrputs("Error: '--extract bed0' requires at least one filename.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
            pc.filter_flags |= kfFilterExtractBed0 | kfFilterNoSplitChr;
            is_interval_bed = 1;
          } else if (strequal_k(cur_modif, "bed1", cur_modif_slen) || strequal_k(cur_modif, "ibed1", cur_modif_slen) || strequal_k(cur_modif, "range", cur_modif_slen)) {
            if (unlikely(param_ct == 1)) {
              logerrputs("Error: '--extract bed1' requires at least one filename.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
            pc.filter_flags |= kfFilterExtractBed1 | kfFilterNoSplitChr;
            is_interval_bed = 1;
          }
          reterr = AllocAndFlatten(&(argvk[arg_idx + 1 + is_interval_bed]), param_ct - is_interval_bed, kPglFnamesize, &pc.extract_fnames);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.filter_flags |= kfFilterPvarReq;
        } else if (strequal_k_unsafe(flagname_p2, "xtract-intersect")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 0x7fffffff))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          const uint32_t cur_modif_slen = strlen(cur_modif);
          uint32_t is_interval_bed = 0;
          if (strequal_k(cur_modif, "bed0", cur_modif_slen)) {
            if (unlikely(param_ct == 1)) {
              logerrputs("Error: '--extract-intersect bed0' requires at least one filename.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
            pc.filter_flags |= kfFilterExtractIntersectBed0 | kfFilterNoSplitChr;
            is_interval_bed = 1;
          } else if (strequal_k(cur_modif, "bed1", cur_modif_slen)) {
            if (unlikely(param_ct == 1)) {
              logerrputs("Error: '--extract-intersect bed1' requires at least one filename.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
            pc.filter_flags |= kfFilterExtractIntersectBed1 | kfFilterNoSplitChr;
            is_interval_bed = 1;
          }
          reterr = AllocAndFlatten(&(argvk[arg_idx + 1 + is_interval_bed]), param_ct - is_interval_bed, kPglFnamesize, &pc.extract_intersect_fnames);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.filter_flags |= kfFilterPvarReq;
        } else if (strequal_k_unsafe(flagname_p2, "xclude")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 0x7fffffff))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          const uint32_t cur_modif_slen = strlen(cur_modif);
          uint32_t is_interval_bed = 0;
          if (strequal_k(cur_modif, "bed0", cur_modif_slen) || strequal_k(cur_modif, "ibed0", cur_modif_slen)) {
            if (unlikely(param_ct == 1)) {
              logerrputs("Error: '--exclude bed0' requires at least one filename.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
            pc.filter_flags |= kfFilterExcludeBed0 | kfFilterNoSplitChr;
            is_interval_bed = 1;
          } else if (strequal_k(cur_modif, "bed1", cur_modif_slen) || strequal_k(cur_modif, "ibed1", cur_modif_slen) || strequal_k(cur_modif, "range", cur_modif_slen)) {
            if (unlikely(param_ct == 1)) {
              logerrputs("Error: '--exclude bed1' requires at least one filename.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
            pc.filter_flags |= kfFilterExcludeBed1 | kfFilterNoSplitChr;
            is_interval_bed = 1;
          }
          reterr = AllocAndFlatten(&(argvk[arg_idx + 1 + is_interval_bed]), param_ct - is_interval_bed, kPglFnamesize, &pc.exclude_fnames);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.filter_flags |= kfFilterPvarReq;
        } else if (strequal_k_unsafe(flagname_p2, "xport") || strequal_k_unsafe(flagname_p2, "xport ped")) {
          // todo: determine actual limit
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 50))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          uint32_t format_param_idxs = 0;
          if (!flagname_p2[5]) {
            GetExportfTargets(&(argvk[arg_idx]), param_ct, &pc.exportf_info.flags, &pc.exportf_info.idpaste_flags, &format_param_idxs);
            if (unlikely(!format_param_idxs)) {
              logerrputs("Error: --export requires at least one output format.  (Did you forget 'ped' or\n'vcf'?)\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
            logputsb();
          } else {
            pc.exportf_info.flags = kfExportfPed;
          }
          // can't have e.g. bgen-1.1 and bgen-1.2 simultaneously, since they
          // have the same extension and different content.
          const uint64_t bgen_flags = S_CAST(uint64_t, pc.exportf_info.flags & (kfExportfBgen11 | kfExportfBgen12 | kfExportfBgen13));
          if (unlikely(bgen_flags & (bgen_flags - 1))) {
            logerrputs("Error: Multiple --export bgen versions.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          if (unlikely((pc.exportf_info.flags & (kfExportfHaps | kfExportfHapsLegend)) == (kfExportfHaps | kfExportfHapsLegend))) {
            logerrputs("Error: 'haps' and 'hapslegend' formats cannot be exported simultaneously.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          if (unlikely((pc.exportf_info.flags & (kfExportfA | kfExportfAD)) == (kfExportfA | kfExportfAD))) {
            logerrputs("Error: 'A' and 'AD' formats cannot be exported simultaneously.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          for (uint32_t param_idx = 1; param_idx <= param_ct; ++param_idx) {
            // could use AdvBoundedTo0Bit()...
            if ((format_param_idxs >> param_idx) & 1) {
              continue;
            }
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (StrStartsWith(cur_modif, "id-paste=", cur_modif_slen)) {
              if (unlikely(!(pc.exportf_info.flags & (kfExportfVcf | kfExportfBgen12 | kfExportfBgen13)))) {
                // todo: bcf
                logerrputs("Error: The 'id-paste' modifier only applies to --export's vcf, bgen-1.2, and\nbgen-1.3 output formats.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              if (unlikely(pc.exportf_info.idpaste_flags)) {
                logerrputs("Error: Multiple --export id-paste= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              reterr = ParseColDescriptor(&(cur_modif[strlen("id-paste=")]), "maybefid\0fid\0iid\0maybesid\0sid\0", "export", kfIdpasteMaybefid, kfIdpasteDefault, 1, &pc.exportf_info.idpaste_flags);
              if (unlikely(reterr)) {
                goto main_ret_1;
              }
            } else if (StrStartsWith(cur_modif, "id-delim=", cur_modif_slen)) {
              if (unlikely(!(pc.exportf_info.flags & (kfExportfVcf | kfExportfBgen12 | kfExportfBgen13)))) {
                logerrputs("Error: The 'id-delim' modifier only applies to --export's vcf, bgen-1.2, and\nbgen-1.3 output formats.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              if (unlikely(pc.exportf_info.id_delim)) {
                logerrputs("Error: Multiple --export id-delim= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              pc.exportf_info.id_delim = ExtractCharParam(&(cur_modif[strlen("id-delim=")]));
              if (unlikely(!pc.exportf_info.id_delim)) {
                logerrputs("Error: --export id-delim= value must be a single character.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              if (unlikely((ctou32(pc.exportf_info.id_delim) < ' ') || (pc.exportf_info.id_delim == '0'))) {
                logerrputs("Error: --export id-delim= value cannot be tab, newline, '0', or a nonprinting\ncharacter.\n");
                goto main_ret_INVALID_CMDLINE;
              }
            } else if (StrStartsWith(cur_modif, "vcf-dosage=", cur_modif_slen)) {
              if (unlikely(!(pc.exportf_info.flags & kfExportfVcf))) {
                logerrputs("Error: The 'vcf-dosage' modifier only applies to --export's vcf output format.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              if (unlikely(pc.exportf_info.vcf_mode != kVcfExport0)) {
                logerrputs("Error: Multiple --export vcf-dosage= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              const char* vcf_dosage_start = &(cur_modif[strlen("vcf-dosage=")]);
              const uint32_t vcf_dosage_start_slen = strlen(vcf_dosage_start);
              if (strequal_k(vcf_dosage_start, "GP", vcf_dosage_start_slen)) {
                if (pc.exportf_info.flags & kfExportfVcf42) {
                  logerrputs("Error: --export vcf-dosage=GP cannot be used in vcf-4.2 mode.\n");
                  goto main_ret_INVALID_CMDLINE_A;
                }
                pc.exportf_info.vcf_mode = kVcfExportGp;
              } else if (strequal_k(vcf_dosage_start, "DS", vcf_dosage_start_slen)) {
                pc.exportf_info.vcf_mode = kVcfExportDs;
              } else if (strequal_k(vcf_dosage_start, "DS-force", vcf_dosage_start_slen)) {
                pc.exportf_info.vcf_mode = kVcfExportDsForce;
              } else if (strequal_k(vcf_dosage_start, "HDS", vcf_dosage_start_slen)) {
                pc.exportf_info.vcf_mode = kVcfExportHds;
              } else if (likely(strequal_k(vcf_dosage_start, "HDS-force", vcf_dosage_start_slen))) {
                pc.exportf_info.vcf_mode = kVcfExportHdsForce;
              } else {
                snprintf(g_logbuf, kLogbufSize, "Error: Invalid --export vcf-dosage= parameter '%s'.\n", vcf_dosage_start);
                goto main_ret_INVALID_CMDLINE_WWA;
              }
            } else if (StrStartsWith(cur_modif, "bits=", cur_modif_slen)) {
              if (unlikely(!(pc.exportf_info.flags & (kfExportfBgen12 | kfExportfBgen13)))) {
                logerrputs("Error: The 'bits' modifier only applies to --export's bgen-1.2 and bgen-1.3\noutput formats.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              if (unlikely(pc.exportf_info.bgen_bits)) {
                logerrputs("Error: Multiple --export bits= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              const char* bits_start = &(cur_modif[strlen("bits=")]);
              uint32_t bgen_bits;
              if (unlikely(ScanPosintCappedx(bits_start, 24, &bgen_bits))) {
                snprintf(g_logbuf, kLogbufSize, "Error: Invalid --export bits= parameter '%s'.\n", bits_start);
                goto main_ret_INVALID_CMDLINE_WWA;
              }
              if (bgen_bits > 16) {
                // okay, bits=24 should eventually be permitted because of
                // multiallelic-variant dosage precision issues
                logerrputs("Error: Current --export bits= maximum is 16.\n");
                reterr = kPglRetNotYetSupported;
                goto main_ret_1;
              }
              if (bgen_bits & (bgen_bits - 1)) {
                logerrputs("Warning: Support for most non-power-of-2 bits= export values is likely to be\ndiscontinued, since .bgen size tends to be larger than the\nnext-higher-power-of-2 precision level.\n");
              }
              pc.exportf_info.bgen_bits = bgen_bits;
            } else if (strequal_k(cur_modif, "include-alt", cur_modif_slen)) {
              if (unlikely(!(pc.exportf_info.flags & (kfExportfA | kfExportfAD)))) {
                logerrputs("Error: The 'include-alt' modifier only applies to --export's A and AD output\nformats.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              pc.exportf_info.flags |= kfExportfIncludeAlt;
            } else if (strequal_k(cur_modif, "omit-nonmale-y", cur_modif_slen)) {
              if (unlikely(!(pc.exportf_info.flags & (kfExportfList | kfExportfRlist)))) {
                logerrputs("Error: The 'omit-nonmale-y' modifier only applies to --export's list and rlist\noutput formats.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              pc.exportf_info.flags |= kfExportfOmitNonmaleY;
            } else if (strequal_k(cur_modif, "01", cur_modif_slen) || strequal_k(cur_modif, "12", cur_modif_slen)) {
              if (unlikely(pc.exportf_info.flags & (kfExportfA | kfExportfAD))) {
                snprintf(g_logbuf, kLogbufSize, "Error: The '%s' modifier does not apply to --export's A and AD output formats.\n", cur_modif);
                goto main_ret_INVALID_CMDLINE_2A;
              }
              if (unlikely(pc.exportf_info.flags & kfExportfVcf)) {
                logerrputs("Error: '01'/'12' cannot be used with --export's vcf output format.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              if (cur_modif[0] == '0') {
                if (unlikely(pc.exportf_info.flags & kfExportf12)) {
                  logerrputs("Error: --export '01' and '12' cannot be used together.\n");
                  goto main_ret_INVALID_CMDLINE;
                }
                pc.exportf_info.flags |= kfExportf01;
              } else {
                if (unlikely(pc.exportf_info.flags & kfExportf01)) {
                  logerrputs("Error: --export '01' and '12' cannot be used together.\n");
                  goto main_ret_INVALID_CMDLINE;
                }
                pc.exportf_info.flags |= kfExportf12;
              }
            } else if (strequal_k(cur_modif, "bgz", cur_modif_slen)) {
              if (unlikely(!(pc.exportf_info.flags & (kfExportfHaps | kfExportfHapsLegend | kfExportfOxGen | kfExportfVcf)))) {
                logerrputs("Error: The 'bgz' modifier only applies to --export's haps{legend}, oxford, and\nvcf output formats.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              pc.exportf_info.flags |= kfExportfBgz;
            } else if (strequal_k(cur_modif, "spaces", cur_modif_slen)) {
              pc.exportf_info.flags |= kfExportfSpaces;
            } else if (likely(strequal_k(cur_modif, "ref-first", cur_modif_slen))) {
              pc.exportf_info.flags |= kfExportfRefFirst;
            } else if (strequal_k(cur_modif, "gen-gz", cur_modif_slen)) {
              logerrputs("Error: 'gen-gz' modifier retired.  Use '--export oxford bgz' instead.\n");
              goto main_ret_INVALID_CMDLINE_WWA;
            } else if (StrStartsWith(cur_modif, "dosage=", cur_modif_slen)) {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --export parameter '%s'. (Did you mean 'vcf-%s'?)\n", cur_modif, cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --export parameter '%s'.%s\n", cur_modif, ((param_idx == param_ct) && (!outname_end))? " (Did you forget '--out'?)" : "");
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          if (pc.exportf_info.flags & (kfExportfVcf | kfExportfBgen12 | kfExportfBgen13)) {
            if (!pc.exportf_info.idpaste_flags) {
              pc.exportf_info.idpaste_flags = kfIdpasteDefault;
            }
          }
          pc.command_flags1 |= kfCommand1Exportf;
          pc.dependency_flags |= kfFilterAllReq;
        } else if (strequal_k_unsafe(flagname_p2, "xport-allele")) {
          if (unlikely((!(pc.command_flags1 & kfCommand1Exportf)) || (!(pc.exportf_info.flags & (kfExportfA | kfExportfATranspose | kfExportfAD))))) {
            logerrputs("Error: --export-allele must be used with --export A/A-transpose/AD.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocFname(argvk[arg_idx + 1], flagname_p, 0, &pc.exportf_info.export_allele_fname);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
        } else if (strequal_k_unsafe(flagname_p2, "xclude-snp")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = CmdlineAllocString(argvk[arg_idx + 1], argvk[arg_idx], kMaxIdSlen, &pc.varid_exclude_snp);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.filter_flags |= kfFilterPvarReq;
        } else if (strequal_k_unsafe(flagname_p2, "xclude-snps")) {
          reterr = ParseNameRanges(&(argvk[arg_idx]), errstr_append, param_ct, 0, range_delim, &pc.exclude_snps_range_list);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.filter_flags |= kfFilterPvarReq;
        } else if (strequal_k_unsafe(flagname_p2, "xtract-fcol")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 4))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = Alloc2col(&(argvk[arg_idx + 1]), flagname_p, param_ct, &pc.extract_fcol_info.params);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.filter_flags |= kfFilterPvarReq;
        } else if (strequal_k_unsafe(flagname_p2, "xtract-fcol-match")) {
          if (unlikely(!pc.extract_fcol_info.params)) {
            logerrputs("Error: --extract-fcol-match must be used with --extract-fcol.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 0x7fffffff))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocAndFlatten(&(argvk[arg_idx + 1]), param_ct, kMaxIdBlen, &pc.extract_fcol_info.match_flattened);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
        } else if (strequal_k_unsafe(flagname_p2, "xtract-fcol-mismatch")) {
          if (unlikely(!pc.extract_fcol_info.params)) {
            logerrputs("Error: --extract-fcol-mismatch must be used with --extract-fcol.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          // could make this check airtight?  right now, there's no error if
          // the user also specifies "--extract-fcol-min 0"
          if (unlikely((pc.extract_fcol_info.min != 0.0) || (pc.extract_fcol_info.max != DBL_MAX))) {
            logerrputs("Error: --extract-fcol-mismatch cannot be used with --extract-fcol-max or\n--extract-fcol-min.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 0x7fffffff))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocAndFlatten(&(argvk[arg_idx + 1]), param_ct, kMaxIdBlen, &pc.extract_fcol_info.mismatch_flattened);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
        } else if (strequal_k_unsafe(flagname_p2, "xtract-fcol-substr")) {
          if (unlikely((!pc.extract_fcol_info.match_flattened) && (!pc.extract_fcol_info.mismatch_flattened))) {
            logerrputs("Error: --extract-fcol-substr must be used with --extract-fcol-match and/or\n--extract-fcol-mismatch.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          pc.extract_fcol_info.match_substr = 1;
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "xtract-fcol-max")) {
          if (unlikely(!pc.extract_fcol_info.params)) {
            logerrputs("Error: --extract-fcol-match must be used with --extract-fcol.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(pc.extract_fcol_info.match_flattened)) {
            logerrputs("Error: --extract-fcol-max cannot be used with --extract-fcol-match.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          if (unlikely(!ScantokDouble(cur_modif, &pc.extract_fcol_info.max))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --extract-fcol-max parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
        } else if (strequal_k_unsafe(flagname_p2, "xtract-fcol-min")) {
          if (unlikely(!pc.extract_fcol_info.params)) {
            logerrputs("Error: --extract-fcol-match must be used with --extract-fcol.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(pc.extract_fcol_info.match_flattened)) {
            logerrputs("Error: --extract-fcol-min cannot be used with --extract-fcol-match.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          if (unlikely(!ScantokDouble(cur_modif, &pc.extract_fcol_info.min))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --extract-fcol-min parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
        } else if (strequal_k_unsafe(flagname_p2, "xtract-if-info")) {
          reterr = ValidateAndAllocCmpExpr(&(argvk[arg_idx + 1]), argvk[arg_idx], param_ct, &pc.extract_if_info_expr);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          // validator doesn't currently check for ';'.  also theoretically
          // possible for '=' to be in key
          if (unlikely(strchr(pc.extract_if_info_expr.pheno_name, ';') || strchr(pc.extract_if_info_expr.pheno_name, '='))) {
            logerrputs("Error: Invalid --extract-if-info expression.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          // LoadPvar() currently checks value string if nonnumeric
          pc.filter_flags |= kfFilterPvarReq;
        } else if (likely(strequal_k_unsafe(flagname_p2, "xclude-if-info"))) {
          reterr = ValidateAndAllocCmpExpr(&(argvk[arg_idx + 1]), argvk[arg_idx], param_ct, &pc.exclude_if_info_expr);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          // validator doesn't currently check for ';'.  also theoretically
          // possible for '=' to be in key
          if (unlikely(strchr(pc.exclude_if_info_expr.pheno_name, ';') || strchr(pc.exclude_if_info_expr.pheno_name, '='))) {
            logerrputs("Error: Invalid --exclude-if-info expression.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          // LoadPvar() currently checks value string if nonnumeric
          pc.filter_flags |= kfFilterPvarReq;
        } else {
          goto main_ret_INVALID_CMDLINE_UNRECOGNIZED;
        }
        break;

      case 'f':
        if (strequal_k_unsafe(flagname_p2, "req")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 5))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          uint32_t bins_only = 0;
          for (uint32_t param_idx = 1; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "zs", cur_modif_slen)) {
              pc.freq_rpt_flags |= kfAlleleFreqZs;
            } else if (strequal_k(cur_modif, "counts", cur_modif_slen)) {
              pc.freq_rpt_flags |= kfAlleleFreqCounts;
            } else if (strequal_k(cur_modif, "case-control", cur_modif_slen)) {
              logerrputs("Error: --freq 'case-control' modifier has been retired.  Use\n--keep-if/--remove-if in conjunction with Unix text-processing utilities\ninstead.\n");
            } else if (StrStartsWith(cur_modif, "cols=", cur_modif_slen)) {
              if (unlikely(pc.freq_rpt_flags & kfAlleleFreqColAll)) {
                logerrputs("Error: Multiple --freq cols= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              reterr = ParseColDescriptor(&(cur_modif[5]), "chrom\0pos\0ref\0alt1\0alt\0reffreq\0alt1freq\0altfreq\0freq\0eq\0eqz\0alteq\0alteqz\0numeq\0altnumeq\0machr2\0minimac3r2\0nobs\0", "freq", kfAlleleFreqColChrom, kfAlleleFreqColDefault, 1, &pc.freq_rpt_flags);
              if (unlikely(reterr)) {
                goto main_ret_1;
              }
              const uint32_t mutually_exclusive_cols = pc.freq_rpt_flags & kfAlleleFreqColMutex;
              if (unlikely(mutually_exclusive_cols & (mutually_exclusive_cols - 1))) {
                logerrputs("Error: --freq's altfreq, freq, eq, eqz, alteq, alteqz, numeq, and altnumeq\ncolumns are mutually exclusive.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              if ((pc.freq_rpt_flags & (kfAlleleFreqColMachR2 | kfAlleleFreqColMinimac3R2)) == (kfAlleleFreqColMachR2 | kfAlleleFreqColMinimac3R2)) {
                logerrputs("Error: --freq machr2 and minimac3r2 columns are mutually exclusive.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
            } else if (strequal_k(cur_modif, "bins-only", cur_modif_slen)) {
              bins_only = 1;
            } else if (likely(
                         StrStartsWith(cur_modif, "refbins=", cur_modif_slen) ||
                         StrStartsWith(cur_modif, "refbins-file=", cur_modif_slen) ||
                         StrStartsWith(cur_modif, "alt1bins=", cur_modif_slen) ||
                         StrStartsWith(cur_modif, "alt1bins-file=", cur_modif_slen))) {
              const uint32_t is_alt1 = (cur_modif[0] == 'a');
              char** binstr_ptr = is_alt1? (&pc.freq_alt1_binstr) : (&pc.freq_ref_binstr);
              if (unlikely(*binstr_ptr)) {
                logerrprintf("Error: Multiple --freq %sbins{-file}= modifiers.\n", is_alt1? "alt1" : "ref");
                goto main_ret_INVALID_CMDLINE;
              }
              if (cur_modif[7 + is_alt1] == '=') {
                // "refbins=", "alt1bins="
                reterr = CmdlineAllocString(&(cur_modif[8 + is_alt1]), is_alt1? "--freq alt1bins=" : "--freq refbins=", 0x7fffffff, binstr_ptr);
              } else {
                // "refbins-file=", "alt1bins-file="
                pc.freq_rpt_flags |= is_alt1? kfAlleleFreqBinsAlt1Fname : kfAlleleFreqBinsRefFname;
                reterr = AllocFname(&(cur_modif[13 + is_alt1]), is_alt1? "freq alt1bins-file=" : "freq refbins-file=", 0, binstr_ptr);
              }
              if (unlikely(reterr)) {
                goto main_ret_1;
              }
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --freq parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          if (bins_only) {
            if (unlikely((!pc.freq_ref_binstr) && (!pc.freq_alt1_binstr))) {
              logerrputs("Error: --freq 'bins-only' must be used with 'refbins{-file}=' and/or\n'alt1bins{-file}='.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
            if (unlikely(pc.freq_rpt_flags & (kfAlleleFreqZs | kfAlleleFreqColAll))) {
              logerrputs("Error: --freq 'bins-only' cannot be used with 'zs' or 'cols=' (which only\naffect the main report).\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
            pc.freq_rpt_flags |= kfAlleleFreqBinsOnly;
          }
          if (!(pc.freq_rpt_flags & kfAlleleFreqColAll)) {
            pc.freq_rpt_flags |= kfAlleleFreqColDefault;
          }
          pc.command_flags1 |= kfCommand1AlleleFreq;
          pc.dependency_flags |= kfFilterAllReq;
        } else if (strequal_k_unsafe(flagname_p2, "rom")) {
          if (unlikely(chr_info.is_include_stack)) {
            logerrputs("Error: --from/--to cannot be used with --autosome{-par} or --{not-}chr.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = CmdlineAllocString(argvk[arg_idx + 1], argvk[arg_idx], kMaxIdSlen, &pc.varid_from);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.filter_flags |= kfFilterPvarReq | kfFilterNoSplitChr;
        } else if (strequal_k_unsafe(flagname_p2, "rom-bp") ||
                   strequal_k_unsafe(flagname_p2, "rom-kb") ||
                   strequal_k_unsafe(flagname_p2, "rom-mb")) {
          if (unlikely(!CmdlineSingleChr(&chr_info, pc.misc_flags))) {
            logerrputs("Error: --from-bp/-kb/-mb and --to-bp/-kb/-mb must be used with --chr, and only\none chromosome.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (unlikely(pc.from_bp != -1)) {
            logerrputs("Error: Multiple --from-bp/-kb/-mb values.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          // permit negative numbers, to simplify shell script windowing logic
          const char* cur_modif = argvk[arg_idx + 1];
          double dxx;
          if (unlikely(!ScantokDouble(cur_modif, &dxx))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --from-bp/-kb/-mb parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          const char unit_char = flagname_p2[4];
          if (unit_char == 'k') {
            dxx *= 1000;
          } else if (unit_char == 'm') {
            dxx *= 1000000;
          }
          if (dxx <= 0.0) {
            pc.from_bp = 0;
          } else {
            // logical to round up rather than down here (this is actually a
            // change from v1.9)
            // don't use ceil() since e.g. ceil(0.001015 * 1000000) is 1016
            if (unlikely(dxx > 2147483646.0)) {
              logerrprintf("Error: --from-bp/-kb/-mb parameter '%s' too large.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_A;
            }
            pc.from_bp = 1 + S_CAST(int32_t, dxx * (1 - kSmallEpsilon));
          }
          pc.filter_flags |= kfFilterPvarReq | kfFilterNoSplitChr;
        } else if (strequal_k_unsafe(flagname_p2, "orce-intersect")) {
          permit_multiple_inclusion_filters = 1;
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "amily")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (param_ct) {
            const char* cur_modif = argvk[arg_idx + 1];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (unlikely(IsReservedPhenoName(cur_modif, cur_modif_slen))) {
              snprintf(g_logbuf, kLogbufSize, "Error: '%s' cannot be used as a categorical phenotype name.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_2A;
            }
            reterr = CmdlineAllocString(cur_modif, argvk[arg_idx], kMaxIdSlen, &pc.catpheno_name);
            if (unlikely(reterr)) {
              goto main_ret_1;
            }
          }
          pc.misc_flags |= kfMiscCatPhenoFamily;
          pc.dependency_flags |= kfFilterPsamReq;
        } else if (strequal_k_unsafe(flagname_p2, "a")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocFname(argvk[arg_idx + 1], flagname_p, 0, &pc.fa_fname);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
        } else if (likely(strequal_k_unsafe(flagname_p2, "amily-missing-catname"))) {
          if (unlikely(!(pc.misc_flags & kfMiscCatPhenoFamily))) {
            logerrputs("Error: --family-missing-catname must be used with --family.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = CmdlineAllocString(argvk[arg_idx + 1], argvk[arg_idx], kMaxIdSlen, &pc.family_missing_catname);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
        } else if (strequal_k_unsafe(flagname_p2, "ilter-cases") || strequal_k_unsafe(flagname_p2, "ilter-controls")) {
          logerrputs("Error: --filter-cases and --filter-controls have been retired.  Use\n--keep-if/--remove-if instead.\n");
          goto main_ret_INVALID_CMDLINE_A;
        } else if (strequal_k_unsafe(flagname_p2, "rqx") || strequal_k_unsafe(flagname_p2, "reqx")) {
          logerrputs("Error: --freqx has been retired.  Use --geno-counts instead.\n");
          goto main_ret_INVALID_CMDLINE_A;
        } else {
          goto main_ret_INVALID_CMDLINE_UNRECOGNIZED;
        }
        break;

      case 'g':
        if (strequal_k_unsafe(flagname_p2, "eno")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          uint32_t geno_thresh_present = 0;
          for (uint32_t param_idx = 1; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            if (!strcmp(cur_modif, "dosage")) {
              pc.misc_flags |= kfMiscGenoDosage;
            } else if (!strcmp(cur_modif, "hh-missing")) {
              pc.misc_flags |= kfMiscGenoHhMissing;
            } else if (unlikely(geno_thresh_present)) {
              logerrputs("Error: Invalid --geno parameter sequence.\n");
              goto main_ret_INVALID_CMDLINE_A;
            } else if (unlikely(!ScantokDouble(cur_modif, &pc.geno_thresh))) {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --geno parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            } else if (unlikely((pc.geno_thresh < 0.0) || (pc.geno_thresh > 1.0))) {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --geno parameter '%s' (must be in [0, 1]).\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            } else {
              geno_thresh_present = 1;
            }
          }
          if (!geno_thresh_present) {
            pc.geno_thresh = 0.1;
          }
          if (pc.geno_thresh < 1.0) {
            pc.filter_flags |= kfFilterPvarReq;
            pc.dependency_flags = kfFilterAllReq | kfFilterNoSplitChr;
          }
        } else if (strequal_k_unsafe(flagname_p2, "eno-counts")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          for (uint32_t param_idx = 1; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "zs", cur_modif_slen)) {
              pc.geno_counts_flags |= kfGenoCountsZs;
            } else if (likely(StrStartsWith(cur_modif, "cols=", cur_modif_slen))) {
              if (unlikely(pc.geno_counts_flags & kfGenoCountsColAll)) {
                logerrputs("Error: Multiple --geno-counts cols= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              reterr = ParseColDescriptor(&(cur_modif[5]), "chrom\0pos\0ref\0alt1\0alt\0homref\0refalt1\0refalt\0homalt1\0altxy\0xy\0hapref\0hapalt1\0hapalt\0hap\0numeq\0missing\0nobs\0", "geno-counts", kfGenoCountsColChrom, kfGenoCountsColDefault, 1, &pc.geno_counts_flags);
              if (unlikely(reterr)) {
                goto main_ret_1;
              }
              if (unlikely((pc.geno_counts_flags & kfGenoCountsColPairex) == kfGenoCountsColPairex)) {
                logerrputs("Error: --geno-counts's hapaltx and hapx columns are mutually exclusive.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              const uint32_t mutually_exclusive_cols = pc.geno_counts_flags & kfGenoCountsColMutex;
              if (unlikely(mutually_exclusive_cols & (mutually_exclusive_cols - 1))) {
                logerrputs("Error: --geno-counts's altxy, xy, and numeq columns are mutually exclusive.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --geno-counts parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          if (!(pc.geno_counts_flags & kfGenoCountsColAll)) {
            pc.geno_counts_flags |= kfGenoCountsColDefault;
          }
          pc.command_flags1 |= kfCommand1GenoCounts;
          pc.dependency_flags |= kfFilterAllReq;
        } else if (strequal_k_unsafe(flagname_p2, "lm")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 18))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          uint32_t explicit_no_firth = 0;
          for (uint32_t param_idx = 1; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "zs", cur_modif_slen)) {
              pc.glm_info.flags |= kfGlmZs;
            } else if (strequal_k(cur_modif, "omit-ref", cur_modif_slen) ||
                       strequal_k(cur_modif, "a0-ref", cur_modif_slen)) {
              pc.glm_info.flags |= kfGlmOmitRef;
            } else if (strequal_k(cur_modif, "sex", cur_modif_slen)) {
              pc.glm_info.flags |= kfGlmSex;
            } else if (strequal_k(cur_modif, "no-x-sex", cur_modif_slen)) {
              pc.glm_info.flags |= kfGlmNoXSex;
            } else if (strequal_k(cur_modif, "log10", cur_modif_slen)) {
              pc.glm_info.flags |= kfGlmLog10;
            } else if (strequal_k(cur_modif, "pheno-ids", cur_modif_slen)) {
              pc.glm_info.flags |= kfGlmPhenoIds;
            } else if (strequal_k(cur_modif, "genotypic", cur_modif_slen)) {
              pc.glm_info.flags |= kfGlmGenotypic;
            } else if (strequal_k(cur_modif, "hethom", cur_modif_slen)) {
              pc.glm_info.flags |= kfGlmHethom;
            } else if (strequal_k(cur_modif, "dominant", cur_modif_slen)) {
              pc.glm_info.flags |= kfGlmDominant;
            } else if (strequal_k(cur_modif, "recessive", cur_modif_slen)) {
              pc.glm_info.flags |= kfGlmRecessive;
            } else if (strequal_k(cur_modif, "interaction", cur_modif_slen)) {
              pc.glm_info.flags |= kfGlmInteraction;
            } else if (strequal_k(cur_modif, "hide-covar", cur_modif_slen)) {
              pc.glm_info.flags |= kfGlmHideCovar;
            } else if (strequal_k(cur_modif, "intercept", cur_modif_slen)) {
              pc.glm_info.flags |= kfGlmIntercept;
            } else if (strequal_k(cur_modif, "no-firth", cur_modif_slen)) {
              explicit_no_firth = 1;
            } else if (strequal_k(cur_modif, "firth-fallback", cur_modif_slen)) {
              pc.glm_info.flags |= kfGlmFirthFallback;
            } else if (strequal_k(cur_modif, "firth", cur_modif_slen)) {
              pc.glm_info.flags |= kfGlmFirth;
            } else if (unlikely(strequal_k(cur_modif, "standard-beta", cur_modif_slen))) {
              logerrputs("Error: --glm 'standard-beta' modifier has been retired.  Use\n--{covar-}variance-standardize instead.\n");
              goto main_ret_INVALID_CMDLINE_A;
            } else if (strequal_k(cur_modif, "perm", cur_modif_slen)) {
              pc.glm_info.flags |= kfGlmPerm;
            } else if (strequal_k(cur_modif, "perm-count", cur_modif_slen)) {
              pc.glm_info.flags |= kfGlmPermCount;
            } else if (StrStartsWith(cur_modif, "cols=", cur_modif_slen)) {
              if (unlikely(pc.glm_info.cols)) {
                logerrputs("Error: Multiple --glm cols= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              reterr = ParseColDescriptor(&(cur_modif[5]), "chrom\0pos\0ref\0alt1\0alt\0ax\0a1count\0totallele\0a1countcc\0totallelecc\0gcountcc\0a1freq\0a1freqcc\0machr2\0firth\0test\0nobs\0beta\0orbeta\0se\0ci\0tz\0p\0err\0", flagname_p, kfGlmColChrom, kfGlmColDefault, 1, &pc.glm_info.cols);
              if (unlikely(reterr)) {
                goto main_ret_1;
              }
              if (unlikely((!(pc.glm_info.cols & (kfGlmColBeta | kfGlmColOrbeta))) && ((pc.glm_info.cols & kfGlmColSe) || ((pc.glm_info.cols & kfGlmColCi) && (pc.ci_size != 0))))) {
                logerrputs("Error: --glm's 'se' and 'ci' columns require beta/orbeta to be included.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
            } else if (StrStartsWith0(cur_modif, "mperm", cur_modif_slen)) {
              if (unlikely((cur_modif_slen < 7) || (cur_modif[5] != '='))) {
                logerrputs("Error: Improper --glm mperm syntax.  (Use --glm mperm=[value]'.)\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              if (unlikely(ScanPosintDefcapx(&(cur_modif[6]), &pc.glm_info.mperm_ct))) {
                snprintf(g_logbuf, kLogbufSize, "Error: Invalid --glm mperm= parameter '%s'.\n", &(cur_modif[6]));
                goto main_ret_INVALID_CMDLINE_WWA;
              }
            } else if (StrStartsWith(cur_modif, "local-covar=", cur_modif_slen)) {
              if (unlikely(pc.glm_local_covar_fname)) {
                logerrputs("Error: Multiple --glm local-covar= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              reterr = AllocFname(&(cur_modif[strlen("local-covar=")]), "glm local-covar=", 0, &pc.glm_local_covar_fname);
              if (unlikely(reterr)) {
                goto main_ret_1;
              }
            } else if (StrStartsWith(cur_modif, "local-bim=", cur_modif_slen) ||
                       StrStartsWith(cur_modif, "local-pvar=", cur_modif_slen)) {
              if (unlikely(pc.glm_local_pvar_fname)) {
                logerrputs("Error: Multiple --glm local-pvar= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              const uint32_t is_pvar = (cur_modif[6] == 'p');
              reterr = AllocFname(&(cur_modif[10 + is_pvar]), "glm local-pvar=", 0, &pc.glm_local_pvar_fname);
              if (unlikely(reterr)) {
                goto main_ret_1;
              }
            } else if (StrStartsWith(cur_modif, "local-fam=", cur_modif_slen) ||
                       StrStartsWith(cur_modif, "local-psam=", cur_modif_slen)) {
              if (unlikely(pc.glm_local_psam_fname)) {
                logerrputs("Error: Multiple --glm local-psam= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              const uint32_t is_psam = (cur_modif[6] == 'p');
              reterr = AllocFname(&(cur_modif[10 + is_psam]), "glm local-psam=", 0, &pc.glm_local_psam_fname);
              if (unlikely(reterr)) {
                goto main_ret_1;
              }
            } else if (strequal_k(cur_modif, "local-omit-last", cur_modif_slen)) {
              pc.glm_info.flags |= kfGlmLocalOmitLast;
            } else if (likely(StrStartsWith(cur_modif, "local-cats=", cur_modif_slen))) {
              if (unlikely(pc.glm_info.local_cat_ct)) {
                logerrputs("Error: Multiple --glm local-cats= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              // bugfix (7 Nov 2017): forgot to offset by strlen("local-cats=")
              if (unlikely(ScanPosintCappedx(&(cur_modif[strlen("local-cats=")]), 4095, &pc.glm_info.local_cat_ct) || (pc.glm_info.local_cat_ct == 1))) {
                logerrputs("Error: Invalid --glm local-cats= category count (must be in [2, 4095]).\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --glm parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          if (!pc.glm_info.cols) {
            pc.glm_info.cols = kfGlmColDefault;
          }
          if (unlikely(explicit_no_firth && (pc.glm_info.flags & (kfGlmFirthFallback | kfGlmFirth)))) {
            logerrputs("Error: Conflicting --glm parameters.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely((pc.glm_info.flags & (kfGlmSex | kfGlmNoXSex)) == (kfGlmSex | kfGlmNoXSex))) {
            logerrputs("Error: Conflicting --glm parameters.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely((pc.glm_info.flags & kfGlmPerm) && pc.glm_info.mperm_ct)) {
            logerrputs("Error: --glm 'perm' and 'mperm=' cannot be used together.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          uint32_t alternate_genotype_col_flags = S_CAST(uint32_t, pc.glm_info.flags & (kfGlmGenotypic | kfGlmHethom | kfGlmDominant | kfGlmRecessive));
          if (alternate_genotype_col_flags) {
            pc.xchr_model = 0;
            if (unlikely(alternate_genotype_col_flags & (alternate_genotype_col_flags - 1))) {
              logerrputs("Error: Conflicting --glm parameters.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
          }
          if (unlikely((pc.glm_info.flags & kfGlmIntercept) && (!(pc.glm_info.cols & kfGlmColTest)))) {
            logerrputs("Error: --glm 'intercept' modifier cannot be used with an omitted 'test' column.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (!pc.glm_local_covar_fname) {
            if (unlikely(pc.glm_local_pvar_fname || pc.glm_local_psam_fname)) {
              logerrputs("Error: Either all three --glm local-covar filenames must be specified, or none\nof them.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
            if (unlikely(pc.glm_info.flags & kfGlmLocalOmitLast)) {
              logerrputs("Error: --glm 'local-omit-last' must be used with 'local-covar='.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
            if (unlikely(pc.glm_info.local_cat_ct)) {
              logerrputs("Error: --glm 'local-cats=' must be used with 'local-covar='.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
          } else {
            if (unlikely((!pc.glm_local_pvar_fname) || (!pc.glm_local_psam_fname))) {
              logerrputs("Error: Either all three --glm local-covar filenames must be specified, or none\nof them.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
          }
          pc.command_flags1 |= kfCommand1Glm;
          pc.dependency_flags |= kfFilterAllReq;
        } else if (strequal_k_unsafe(flagname_p2, "en")) {
          if (unlikely(load_params || (xload & (~kfXloadOxSample)))) {
            goto main_ret_INVALID_CMDLINE_INPUT_CONFLICT;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (param_ct == 2) {
            const char* cur_modif = argvk[arg_idx + 2];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "ref-first", cur_modif_slen)) {
              oxford_import_flags |= kfOxfordImportRefFirst;
            } else if (strequal_k(cur_modif, "ref-unknown", cur_modif_slen)) {
              oxford_import_flags |= kfOxfordImportRefUnknown;
            } else if (likely(
                strequal_k(cur_modif, "ref-last", cur_modif_slen) ||
                strequal_k(cur_modif, "ref-second", cur_modif_slen))) {
              oxford_import_flags |= kfOxfordImportRefLast;
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --gen parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          if (!(oxford_import_flags & kfOxfordImportRefAll)) {
            logerrputs("Warning: No --gen REF/ALT mode specified ('ref-first', 'ref-last', or\n'ref-unknown').  This will be required as of alpha 3.\n");
          }
          const char* cur_fname = argvk[arg_idx + 1];
          const uint32_t slen = strlen(cur_fname);
          if (unlikely(slen > kPglFnamesize - 1)) {
            logerrputs("Error: --gen filename too long.\n");
            goto main_ret_OPEN_FAIL;
          }
          memcpy(pgenname, cur_fname, slen + 1);
          xload |= kfXloadOxGen;
        } else if (likely(strequal_k_unsafe(flagname_p2, "enotyping-rate"))) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (param_ct) {
            const char* cur_modif = argvk[arg_idx + 1];
            if (unlikely(strcmp("dosage", cur_modif))) {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --genotyping-rate parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
            pc.misc_flags |= kfMiscGenotypingRateDosage;
          }
          pc.command_flags1 |= kfCommand1GenotypingRate;
          pc.dependency_flags |= kfFilterAllReq;
        } else {
          goto main_ret_INVALID_CMDLINE_UNRECOGNIZED;
        }
        break;

      case 'h':
        if (strequal_k_unsafe(flagname_p2, "ardy")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 4))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          for (uint32_t param_idx = 1; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "zs", cur_modif_slen)) {
              pc.hardy_flags |= kfHardyZs;
            } else if (strequal_k(cur_modif, "midp", cur_modif_slen)) {
              pc.hardy_flags |= kfHardyMidp;
            } else if (strequal_k(cur_modif, "redundant", cur_modif_slen)) {
              pc.hardy_flags |= kfHardyRedundant;
            } else if (likely(StrStartsWith(cur_modif, "cols=", cur_modif_slen))) {
              if (unlikely(pc.hardy_flags & kfHardyColAll)) {
                logerrputs("Error: Multiple --hardy cols= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              reterr = ParseColDescriptor(&(cur_modif[5]), "chrom\0pos\0ref\0alt1\0alt\0ax\0gcounts\0gcount1col\0hetfreq\0sexaf\0femalep\0p\0", "freq", kfHardyColChrom, kfHardyColDefault, 1, &pc.hardy_flags);
              if (unlikely(reterr)) {
                goto main_ret_1;
              }
              if (unlikely((pc.hardy_flags & (kfHardyColGcounts | kfHardyColGcount1col)) == (kfHardyColGcounts | kfHardyColGcount1col))) {
                logerrputs("Error: --hardy's gcounts and gcounts1col column sets are mutually exclusive.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --hardy parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          if (!(pc.hardy_flags & kfHardyColAll)) {
            pc.hardy_flags |= kfHardyColDefault;
          }
          pc.command_flags1 |= kfCommand1Hardy;
          pc.dependency_flags |= kfFilterAllReq;
        } else if (strequal_k_unsafe(flagname_p2, "we")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 3))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          for (uint32_t param_idx = 1; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            if (!strcmp(cur_modif, "midp")) {
              pc.misc_flags |= kfMiscHweMidp;
            } else if (!strcmp(cur_modif, "keep-fewhet")) {
              pc.misc_flags |= kfMiscHweKeepFewhet;
            } else {
              if (unlikely((pc.hwe_thresh != 0.0) || (!ScantokDouble(cur_modif, &pc.hwe_thresh)))) {
                logerrputs("Error: Invalid --hwe parameter sequence.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              if (unlikely((pc.hwe_thresh < 0.0) || (pc.hwe_thresh >= 1.0))) {
                snprintf(g_logbuf, kLogbufSize, "Error: Invalid --hwe threshold '%s' (must be in [0, 1)).\n", cur_modif);
                goto main_ret_INVALID_CMDLINE_WWA;
              }
            }
          }
          if ((pc.misc_flags & kfMiscHweMidp) && (pc.hwe_thresh >= 0.5)) {
            logerrputs("Error: --hwe threshold must be smaller than 0.5 when using mid-p adjustment.\n");
          }
          pc.filter_flags |= kfFilterPvarReq;
          pc.dependency_flags |= kfFilterAllReq | kfFilterNoSplitChr;
        } else if (strequal_k_unsafe(flagname_p2, "ard-call-threshold")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          double hard_call_frac;
          if (unlikely((!ScantokDouble(cur_modif, &hard_call_frac)) || (hard_call_frac < 0.0) || (hard_call_frac >= (0.5 - kSmallEpsilon)))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --hard-call-threshold parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          pc.hard_call_thresh = S_CAST(int32_t, hard_call_frac * ((1 + kSmallEpsilon) * kDosageMid));
        } else if (strequal_k_unsafe(flagname_p2, "orse")) {
          if (unlikely(chr_info.chrset_source)) {
            logerrputs("Error: Conflicting chromosome-set flags.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          chr_info.chrset_source = kChrsetSourceCmdline;
          chr_info.autosome_ct = 31;
          chr_info.xymt_codes[0] = 32;
          chr_info.xymt_codes[1] = 33;
          chr_info.xymt_codes[2] = UINT32_MAXM1;
          chr_info.xymt_codes[3] = UINT32_MAXM1;
          chr_info.xymt_codes[4] = UINT32_MAXM1;
          chr_info.xymt_codes[5] = UINT32_MAXM1;
#ifdef __LP64__
          chr_info.haploid_mask[0] = 0x300000000LLU;
#else
          chr_info.haploid_mask[0] = 0;
          chr_info.haploid_mask[1] = 3;
#endif
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "uman")) {
          if (unlikely(chr_info.chrset_source)) {
            logerrputs("Error: Conflicting chromosome-set flags.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          chr_info.chrset_source = kChrsetSourceCmdline;
          goto main_param_zero;
        } else if (likely(strequal_k_unsafe(flagname_p2, "aps"))) {
          if (unlikely(load_params || xload)) {
            goto main_ret_INVALID_CMDLINE_INPUT_CONFLICT;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (param_ct == 2) {
            const char* cur_modif = argvk[arg_idx + 2];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "ref-first", cur_modif_slen)) {
              oxford_import_flags |= kfOxfordImportRefFirst;
            } else if (likely(
                strequal_k(cur_modif, "ref-last", cur_modif_slen) ||
                strequal_k(cur_modif, "ref-second", cur_modif_slen))) {
              oxford_import_flags |= kfOxfordImportRefLast;
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --haps parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          const char* cur_fname = argvk[arg_idx + 1];
          const uint32_t slen = strlen(cur_fname);
          if (unlikely(slen > kPglFnamesize - 1)) {
            logerrputs("Error: --haps filename too long.\n");
            goto main_ret_OPEN_FAIL;
          }
          memcpy(pgenname, cur_fname, slen + 1);
          xload |= kfXloadOxHaps;
        } else {
          goto main_ret_INVALID_CMDLINE_UNRECOGNIZED;
        }
        break;

      case 'i':
        if (strequal_k_unsafe(flagname_p2, "ndiv-sort")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 3))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* mode_str = argvk[arg_idx + 1];
          const char first_char_upcase_match = mode_str[0] & 0xdf;
          const uint32_t mode_slen = strlen(mode_str);
          if (strequal_k(mode_str, "0", mode_slen) ||
              strequal_k(mode_str, "none", mode_slen)) {
            pc.sample_sort_flags = kfSortNone;
          } else if (((mode_slen == 1) && (first_char_upcase_match == 'N')) ||
                     strequal_k(mode_str, "natural", mode_slen)) {
            pc.sample_sort_flags = kfSortNatural;
          } else if (((mode_slen == 1) && (first_char_upcase_match == 'A')) ||
                     strequal_k(mode_str, "ascii", mode_slen)) {
            pc.sample_sort_flags = kfSortAscii;
          } else if (likely(
              ((mode_slen == 1) && (first_char_upcase_match == 'F')) ||
              strequal_k(mode_str, "file", mode_slen))) {
            if (unlikely(param_ct == 1)) {
              snprintf(g_logbuf, kLogbufSize, "Error: Missing '--indiv-sort %s' filename.\n", mode_str);
              goto main_ret_INVALID_CMDLINE_2A;
            }
            pc.sample_sort_flags = kfSortFile;
            uint32_t fname_modif_idx = 2;
            if (param_ct == 3) {
              if (unlikely(CheckExtraParam(&(argvk[arg_idx]), "sid", &fname_modif_idx))) {
                goto main_ret_INVALID_CMDLINE_A;
              }
              pc.sample_sort_flags |= kfSortFileSid;
            }
            reterr = AllocFname(argvk[arg_idx + fname_modif_idx], flagname_p, 0, &pc.sample_sort_fname);
            if (unlikely(reterr)) {
              goto main_ret_1;
            }
          } else {
            snprintf(g_logbuf, kLogbufSize, "Error: '%s' is not a valid mode for --indiv-sort.\n", mode_str);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          if (unlikely((param_ct > 1) && (!(pc.sample_sort_flags & kfSortFile)))) {
            snprintf(g_logbuf, kLogbufSize, "Error: '--indiv-sort %s' does not accept additional parameters.\n", mode_str);
            goto main_ret_INVALID_CMDLINE_2A;
          }
        } else if (strequal_k_unsafe(flagname_p2, "d-delim")) {
          if (unlikely(const_fid || (import_flags & kfImportDoubleId))) {
            logerrputs("Error: --id-delim can no longer be used with --const-fid or --double-id.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (param_ct) {
            const char* cur_modif = argvk[arg_idx + 1];
            id_delim = ExtractCharParam(cur_modif);
            if (unlikely(!id_delim)) {
              logerrputs("Error: --id-delim delimiter must be a single character.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
            if (unlikely(ctou32(id_delim) < ' ')) {
              logerrputs("Error: --id-delim parameter cannot be tab, newline, or a nonprinting character.\n");
              goto main_ret_INVALID_CMDLINE;
            }
          }
          if (!id_delim) {
            id_delim = '_';
          }
        } else if (strequal_k_unsafe(flagname_p2, "ndep-pairwise") || strequal_k_unsafe(flagname_p2, "ndep-pairphase")) {
          if (unlikely(pc.command_flags1 & kfCommand1LdPrune)) {
            logerrputs("Error: Multiple LD pruning commands.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 2, 4))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          double first_paramd;
          const char* first_param_end = ScanadvDouble(cur_modif, &first_paramd);
          if (unlikely((!first_param_end) || (first_paramd < 0.0))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --%s window size '%s'.\n", flagname_p, cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          uint32_t is_kb = 0;
          uint32_t next_param_idx = 2;
          if (MatchUpperK(first_param_end, "KB") && (!first_param_end[2])) {
            is_kb = 1;
          } else if (MatchUpperK(argvk[arg_idx + 2], "KB") && (!argvk[arg_idx + 2][2])) {
            is_kb = 1;
            next_param_idx = 3;
          }
          if (is_kb) {
            pc.ld_info.prune_flags |= kfLdPruneWindowBp;
            if (first_paramd > 2147483.646) {
              pc.ld_info.prune_window_size = 2147483646;
            } else {
              pc.ld_info.prune_window_size = S_CAST(int32_t, first_paramd * 1000 * (1 + kSmallEpsilon));
              if (unlikely(pc.ld_info.prune_window_size < 2)) {
                snprintf(g_logbuf, kLogbufSize, "Error: --%s window size cannot be smaller than 2.\n", flagname_p);
                goto main_ret_INVALID_CMDLINE_2A;
              }
            }
          } else {
            if (first_paramd > 2147483647) {
              pc.ld_info.prune_window_size = 2147483647;
            } else {
              pc.ld_info.prune_window_size = S_CAST(int32_t, first_paramd);
            }
          }
          if (unlikely(next_param_idx + 2 == param_ct)) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --%s parameter sequence.\n", flagname_p);
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (next_param_idx < param_ct) {
            // explicit step size
            cur_modif = argvk[arg_idx + next_param_idx];
            if (unlikely(ScanPosintDefcapx(cur_modif, &pc.ld_info.prune_window_incr))) {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --%s window-increment '%s'.\n", flagname_p, cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
            if (!is_kb) {
              if (unlikely(pc.ld_info.prune_window_incr > pc.ld_info.prune_window_size)) {
                snprintf(g_logbuf, kLogbufSize, "Error: --%s window-increment cannot be larger than window size.\n", flagname_p);
                goto main_ret_INVALID_CMDLINE_2A;
              }
            } else if (unlikely(pc.ld_info.prune_window_incr != 1)) {
              snprintf(g_logbuf, kLogbufSize, "Error: --%s window-increment must be 1 when window size is in\nkilobase units.\n", flagname_p);
              goto main_ret_INVALID_CMDLINE_2A;
            }
          } else {
            pc.ld_info.prune_window_incr = 1;
          }
          cur_modif = argvk[arg_idx + param_ct];
          if (unlikely((!ScantokDouble(cur_modif, &pc.ld_info.prune_last_param)) || (pc.ld_info.prune_last_param < 0.0) || (pc.ld_info.prune_last_param >= 1.0))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --%s r^2 threshold '%s'.\n", flagname_p2, cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          pc.command_flags1 |= kfCommand1LdPrune;
          pc.dependency_flags |= kfFilterAllReq;
          if (flagname_p2[9] == 'p') {
            pc.ld_info.prune_flags |= kfLdPrunePairphase;
          } else {
            pc.ld_info.prune_flags |= kfLdPrunePairwise;
          }
        } else if (strequal_k_unsafe(flagname_p2, "nput-missing-genotype")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          input_missing_geno_char = ExtractCharParam(cur_modif);
          if (unlikely(ctou32(input_missing_geno_char) <= ' ')) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --input-missing-genotype parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
        } else if (strequal_k_unsafe(flagname_p2, "nput-missing-phenotype")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          double dxx;
          if (unlikely(ScanInt32x(cur_modif, &pc.missing_pheno) || ((pc.missing_pheno >= 0) && (pc.missing_pheno <= 2)) || (!ScantokDouble(cur_modif, &dxx)) || (dxx != S_CAST(double, pc.missing_pheno)))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --input-missing-phenotype parameter '%s' (must be an integer in [-2147483647, -1] or [3, 2147483647]).\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
        } else if (strequal_k_unsafe(flagname_p2, "mport-dosage-certainty")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          if (unlikely((!ScantokDouble(cur_modif, &import_dosage_certainty)) || (import_dosage_certainty < 0.0) || (import_dosage_certainty > 1.0))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --import-dosage-certainty parameter '%s' (must be in [0, 1]).\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          import_dosage_certainty *= 1.0 - kSmallEpsilon;
          // We may as well enforce --hard-call-threshold +
          // --import-dosage-certainty <= 1.
          uint32_t hard_call_thresh = pc.hard_call_thresh;
          if (hard_call_thresh == UINT32_MAX) {
            hard_call_thresh = kDosageMid / 10;
          }
          if (u31tod(hard_call_thresh) + import_dosage_certainty * kDosageMid >= u31tod(kDosageMid)) {
            logerrputs("Error: --hard-call-threshold + --import-dosage-certainty settings cannot add up\nto more than 1.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
        } else if (strequal_k_unsafe(flagname_p2, "id-sid")) {
          pc.misc_flags |= kfMiscIidSid;
        } else if (likely(strequal_k_unsafe(flagname_p2, "mport-dosage"))) {
          if (unlikely(load_params || xload)) {
            goto main_ret_INVALID_CMDLINE_INPUT_CONFLICT;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 11))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          uint32_t format_num_m1 = 4;
          for (uint32_t param_idx = 2; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "noheader", cur_modif_slen)) {
              plink1_dosage_info.flags |= kfPlink1DosageNoheader;
            } else if ((cur_modif_slen > 6) && StrStartsWithUnsafe(cur_modif, "skip") && (cur_modif[4] >= '0') && (cur_modif[4] <= '2') && (cur_modif[5] == '=')) {
              const uint32_t skip_idx = ctou32(cur_modif[4]) - 48;
              if (unlikely(plink1_dosage_info.skips[skip_idx])) {
                logerrprintf("Error: Multiple --import-dosage skip%u= modifiers.\n", skip_idx);
                goto main_ret_INVALID_CMDLINE;
              }
              if (unlikely(ScanUintCappedx(&(cur_modif[6]), kMaxLongLine / 2, &(plink1_dosage_info.skips[skip_idx])))) {
                snprintf(g_logbuf, kLogbufSize, "Error: Invalid --import-dosage skip%u= parameter '%s'.\n", skip_idx, &(cur_modif[6]));
                goto main_ret_INVALID_CMDLINE_WWA;
              }
            } else if (strequal_k(cur_modif, "dose1", cur_modif_slen)) {
              plink1_dosage_info.flags |= kfPlink1DosageFormatSingle01;
            } else if (StrStartsWithUnsafe(cur_modif, "format=")) {
              // strequal_k() and StrStartsWith() both suboptimal here
              if (unlikely(format_num_m1 != 4)) {
                logerrputs("Error: Multiple --import-dosage format= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              if (cur_modif_slen == 8) {
                format_num_m1 = ctou32(cur_modif[7]) - 49;
                if (unlikely(format_num_m1 >= 3)) {
                  snprintf(g_logbuf, kLogbufSize, "Error: Invalid --import-dosage format= parameter '%c'.\n", cur_modif[7]);
                  goto main_ret_INVALID_CMDLINE_2A;
                }
              } else if (likely(strequal_k(&(cur_modif[7]), "infer", cur_modif_slen - 7))) {
                format_num_m1 = 3;
              } else {
                snprintf(g_logbuf, kLogbufSize, "Error: Invalid --import-dosage format= parameter '%s'.\n", &(cur_modif[7]));
                goto main_ret_INVALID_CMDLINE_WWA;
              }
            } else if (strequal_k(cur_modif, "ref-first", cur_modif_slen)) {
              plink1_dosage_info.flags |= kfPlink1DosageRefFirst;
            } else if (strequal_k(cur_modif, "ref-last", cur_modif_slen) ||
                       strequal_k(cur_modif, "ref-second", cur_modif_slen)) {
              plink1_dosage_info.flags |= kfPlink1DosageRefLast;
            } else if (StrStartsWith(cur_modif, "id-delim=", cur_modif_slen)) {
              if (unlikely(plink1_dosage_info.id_delim)) {
                logerrputs("Error: Multiple --import-dosage id-delim= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              const char* id_delim_str = &(cur_modif[strlen("id-delim=")]);
              char cc = ExtractCharParam(id_delim_str);
              if (unlikely(!cc)) {
                snprintf(g_logbuf, kLogbufSize, "Error: Invalid --import-dosage id-delim= parameter '%s'.\n", id_delim_str);
                goto main_ret_INVALID_CMDLINE_WWA;
              }
              plink1_dosage_info.id_delim = cc;
            } else if (StrStartsWith(cur_modif, "single-chr=", cur_modif_slen)) {
              if (unlikely(import_single_chr_str)) {
                logerrputs("Error: Multiple --import-dosage single-chr= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              const char* chr_code = &(cur_modif[strlen("single-chr=")]);
              if (!(pc.misc_flags & kfMiscAllowExtraChrs)) {
                if (unlikely(IsI32Neg(GetChrCodeRaw(chr_code)))) {
                  snprintf(g_logbuf, kLogbufSize, "Error: Invalid --import-dosage single-chr= chromosome code '%s'. (Did you forget --allow-extra-chr?)\n", chr_code);
                  goto main_ret_INVALID_CMDLINE_WWA;
                }
              }
              reterr = CmdlineAllocString(chr_code, argvk[arg_idx], kMaxIdSlen, &import_single_chr_str);
              if (unlikely(reterr)) {
                goto main_ret_1;
              }
            } else if (StrStartsWith(cur_modif, "chr-col-num=", cur_modif_slen)) {
              if (unlikely(plink1_dosage_info.chr_col_idx != UINT32_MAX)) {
                logerrputs("Error: Multiple --import-dosage chr-col-num= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              const char* chr_col_num_start = &(cur_modif[strlen("chr-col-num=")]);
              uint32_t uii;
              if (unlikely(ScanPosintCappedx(chr_col_num_start, kMaxLongLine / 2, &uii))) {
                snprintf(g_logbuf, kLogbufSize, "Error: Invalid --import-dosage chr-col-num= parameter '%s'.\n", chr_col_num_start);
                goto main_ret_INVALID_CMDLINE_WWA;
              }
              plink1_dosage_info.chr_col_idx = uii - 1;
            } else if (likely(StrStartsWith(cur_modif, "pos-col-num=", cur_modif_slen))) {
              if (unlikely(plink1_dosage_info.pos_col_idx != UINT32_MAX)) {
                logerrputs("Error: Multiple --import-dosage pos-col-num= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              const char* pos_col_num_start = &(cur_modif[strlen("pos-col-num=")]);
              uint32_t uii;
              if (unlikely(ScanPosintCappedx(pos_col_num_start, kMaxLongLine / 2, &uii))) {
                snprintf(g_logbuf, kLogbufSize, "Error: Invalid --import-dosage pos-col-num= parameter '%s'.\n", pos_col_num_start);
                goto main_ret_INVALID_CMDLINE_WWA;
              }
              plink1_dosage_info.pos_col_idx = uii - 1;
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --import-dosage parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }

          if (!format_num_m1) {
            plink1_dosage_info.flags |= kfPlink1DosageFormatSingle;
          } else {
            if (plink1_dosage_info.flags & kfPlink1DosageFormatSingle01) {
              if (unlikely(format_num_m1 != 3)) {
                logerrputs("Error: --import-dosage 'dose1' modifier must be used with 'format=1'.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              // format_num_m1 = 0;
              plink1_dosage_info.flags |= kfPlink1DosageFormatSingle;
            } else {
              if (format_num_m1 == 1) {
                plink1_dosage_info.flags |= kfPlink1DosageFormatDouble;
              } else if (format_num_m1 == 2) {
                plink1_dosage_info.flags |= kfPlink1DosageFormatTriple;
              }
            }
          }
          if (unlikely((plink1_dosage_info.flags & (kfPlink1DosageRefFirst | kfPlink1DosageRefLast)) == (kfPlink1DosageRefFirst | kfPlink1DosageRefLast))) {
            logerrputs("Error: --import-dosage 'ref-first' and 'ref-last' modifiers cannot be used\ntogether.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          const uint32_t id_col_idx = plink1_dosage_info.skips[0];
          const uint32_t a1_col_idx = id_col_idx + plink1_dosage_info.skips[1] + 1;
          const uint32_t data_col_idx = a1_col_idx + plink1_dosage_info.skips[2] + 2;
          const uint32_t chr_col_idx = plink1_dosage_info.chr_col_idx;
          if (chr_col_idx != UINT32_MAX) {
            if (unlikely(import_single_chr_str)) {
              logerrputs("Error: --import-dosage 'single-chr=' and 'chr-col-num=' modifiers cannot be\nused together.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
            if (unlikely((chr_col_idx == id_col_idx) || (chr_col_idx == a1_col_idx) || (chr_col_idx == a1_col_idx + 1))) {
              logerrputs("Error: --import-dosage chr-col-num= value collides with another column.\n");
              goto main_ret_INVALID_CMDLINE_A;
            } else if (unlikely(chr_col_idx >= data_col_idx)) {
              logerrputs("Error: --import-dosage chr-col-num= value too large.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
          }
          const uint32_t pos_col_idx = plink1_dosage_info.pos_col_idx;
          if (pos_col_idx != UINT32_MAX) {
            if (unlikely((pos_col_idx == id_col_idx) || (pos_col_idx == a1_col_idx) || (pos_col_idx == a1_col_idx + 1) || (pos_col_idx == chr_col_idx))) {
              logerrputs("Error: --import-dosage pos-col-num= value collides with another column.\n");
              goto main_ret_INVALID_CMDLINE_A;
            } else if (unlikely(pos_col_idx >= data_col_idx)) {
              logerrputs("Error: --import-dosage pos-col-num= value too large.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
          }
          const char* cur_modif = argvk[arg_idx + 1];
          const uint32_t slen = strlen(cur_modif);
          if (unlikely(slen > kPglFnamesize - 1)) {
            logerrputs("Error: --import-dosage filename too long.\n");
            goto main_ret_OPEN_FAIL;
          }
          memcpy(pgenname, cur_modif, slen + 1);
          xload = kfXloadPlink1Dosage;
        } else {
          goto main_ret_INVALID_CMDLINE_UNRECOGNIZED;
        }
        break;

      case 'k':
        if (strequal_k_unsafe(flagname_p2, "eep")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 0x7fffffff))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocAndFlatten(&(argvk[arg_idx + 1]), param_ct, kPglFnamesize, &pc.keep_fnames);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.filter_flags |= kfFilterPsamReq;
        } else if (strequal_k_unsafe(flagname_p2, "eep-fam")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 0x7fffffff))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocAndFlatten(&(argvk[arg_idx + 1]), param_ct, kPglFnamesize, &pc.keepfam_fnames);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.filter_flags |= kfFilterPsamReq;
        } else if (strequal_k_unsafe(flagname_p2, "eep-autoconv")) {
          import_flags |= kfImportKeepAutoconv;
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "eep-females")) {
          pc.filter_flags |= kfFilterPsamReq | kfFilterExclMales | kfFilterExclNosex;
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "eep-males")) {
          pc.filter_flags |= kfFilterPsamReq | kfFilterExclFemales | kfFilterExclNosex;
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "eep-nosex")) {
          pc.filter_flags |= kfFilterPsamReq | kfFilterExclFemales | kfFilterExclMales;
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "eep-founders")) {
          pc.filter_flags |= kfFilterPsamReq | kfFilterExclNonfounders;
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "eep-nonfounders")) {
          if (unlikely(pc.filter_flags & kfFilterExclNonfounders)) {
            logerrputs("Error: --keep-nonfounders cannot be used with --keep-founders.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          pc.filter_flags |= kfFilterPsamReq | kfFilterExclFounders;
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "ing-cutoff")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (param_ct == 2) {
            // .king.id, .king.bin appended
            reterr = AllocFname(argvk[arg_idx + 1], flagname_p, 9, &king_cutoff_fprefix);
            if (unlikely(reterr)) {
              goto main_ret_1;
            }
            pc.dependency_flags |= kfFilterPsamReq;
          } else {
            pc.dependency_flags |= kfFilterAllReq;
          }
          const char* cur_modif = argvk[arg_idx + param_ct];
          if (unlikely((!ScantokDouble(cur_modif, &pc.king_cutoff)) || (pc.king_cutoff < 0.0) || (pc.king_cutoff >= 0.5))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --king-cutoff parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          pc.command_flags1 |= kfCommand1KingCutoff;
        } else if (strequal_k_unsafe(flagname_p2, "ing-table-filter")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          if (unlikely((!ScantokDouble(cur_modif, &pc.king_table_filter)) || (pc.king_table_filter > 0.5))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --king-table-filter parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
        } else if (strequal_k_unsafe(flagname_p2, "ing-table-subset")) {
          if (unlikely(pc.king_cutoff != -1)) {
            logerrputs("Error: --king-table-subset cannot be used with --king-cutoff.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocFname(argvk[arg_idx + 1], flagname_p, 0, &pc.king_table_subset_fname);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          if (param_ct == 2) {
            const char* cur_modif = argvk[arg_idx + 2];
            if (unlikely((!ScantokDouble(cur_modif, &pc.king_table_subset_thresh)) || (pc.king_table_subset_thresh > 0.5))) {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --king-table-subset threshold '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          } else {
            pc.king_table_subset_thresh = -DBL_MAX;
          }
        } else if (strequal_k_unsafe(flagname_p2, "eep-if")) {
          reterr = ValidateAndAllocCmpExpr(&(argvk[arg_idx + 1]), argvk[arg_idx], param_ct, &pc.keep_if_expr);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.filter_flags |= kfFilterPsamReq;
        } else if (strequal_k_unsafe(flagname_p2, "eep-cats")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocFname(argvk[arg_idx + 1], flagname_p, 0, &pc.keep_cats_fname);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.filter_flags |= kfFilterPsamReq;
        } else if (strequal_k_unsafe(flagname_p2, "eep-cat-names")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 0x7fffffff))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocAndFlatten(&(argvk[arg_idx + 1]), param_ct, kMaxIdBlen, &pc.keep_cat_names_flattened);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.filter_flags |= kfFilterPsamReq;
        } else if (strequal_k_unsafe(flagname_p2, "eep-cat-pheno")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = CmdlineAllocString(argvk[arg_idx + 1], argvk[arg_idx], kMaxIdSlen, &pc.keep_cat_phenoname);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
        } else if (strequal_k_unsafe(flagname_p2, "eep-fcol")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 2, 0x7fffffff))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocFname(argvk[arg_idx + 1], flagname_p, 0, &pc.keep_fcol_fname);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          reterr = AllocAndFlatten(&(argvk[arg_idx + 2]), param_ct - 1, kMaxIdBlen, &pc.keep_fcol_flattened);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.filter_flags |= kfFilterPsamReq;
        } else if (strequal_k_unsafe(flagname_p2, "eep-fcol-name")) {
          if (unlikely(!pc.keep_fcol_fname)) {
            logerrputs("Error: --keep-fcol-name must be used with --keep-fcol.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = CmdlineAllocString(argvk[arg_idx + 1], argvk[arg_idx], kMaxIdSlen, &pc.keep_fcol_name);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
        } else if (strequal_k_unsafe(flagname_p2, "eep-fcol-num")) {
          if (unlikely(!pc.keep_fcol_fname)) {
            logerrputs("Error: --keep-fcol-num must be used with --keep-fcol.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(pc.keep_fcol_name)) {
            logerrputs("Error: --keep-fcol-num can't be used with --keep-fcol-name.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          if (unlikely(ScanPosintDefcapx(cur_modif, &pc.keep_fcol_num) || (pc.keep_fcol_num == 1))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --keep-fcol-num parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
        } else if (likely(strequal_k_unsafe(flagname_p2, "eep-allele-order"))) {
          if (unlikely((pc.command_flags1 & kfCommand1Glm) && (!(pc.glm_info.flags & kfGlmOmitRef)))) {
            // update (17 May 2018): Best to error out instead of ignore if
            // this is used with --linear/--logistic/--glm without 'omit-ref',
            // since in that case the user probably wants to add 'omit-ref'.
            logerrputs("Error: To make --glm always test ALT alleles, you must use --glm's 'omit-ref'\nmodifier, not --keep-allele-order.  (--keep-allele-order no longer has any\neffect, since plink2 always keeps track of REF/ALT alleles; but --glm defaults\nto testing minor instead of ALT alleles, since this can be necessary for\navoiding multicollinearity.)\n");
            goto main_ret_INVALID_CMDLINE;
          }
          logputs("Note: --keep-allele-order no longer has any effect.\n");
          goto main_param_zero;
        } else {
          goto main_ret_INVALID_CMDLINE_UNRECOGNIZED;
        }
        break;

      case 'l':
        if (strequal_k_unsafe(flagname_p2, "ambda")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          double lambda;
          if (unlikely(!ScantokDouble(argvk[arg_idx + 1], &lambda))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --lambda parameter '%s'.\n", argvk[arg_idx + 1]);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          if (lambda < 1.0) {
            logputs("Note: --lambda parameter set to 1.\n");
            lambda = 1.0;
          }
          pc.adjust_info.lambda = lambda;
          adjust_file_info.base.lambda = lambda;
        } else if (strequal_k_unsafe(flagname_p2, "egend")) {
          if (unlikely(load_params || (xload & (~kfXloadOxHaps)))) {
            goto main_ret_INVALID_CMDLINE_INPUT_CONFLICT;
          }
          if (unlikely(!xload)) {
            logerrputs("Error: --legend must be used with --haps.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 2, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_fname = argvk[arg_idx + 1];
          uint32_t slen = strlen(cur_fname);
          if (unlikely(slen > kPglFnamesize - 1)) {
            logerrputs("Error: --legend filename too long.\n");
            goto main_ret_OPEN_FAIL;
          }
          memcpy(pvarname, cur_fname, slen + 1);
          const char* chr_code = argvk[arg_idx + 2];
          if (!(pc.misc_flags & kfMiscAllowExtraChrs)) {
            if (unlikely(IsI32Neg(GetChrCodeRaw(chr_code)))) {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --legend chromosome code '%s'. (Did you forget --allow-extra-chr?)\n", chr_code);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          reterr = CmdlineAllocString(chr_code, argvk[arg_idx], kMaxIdSlen, &import_single_chr_str);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          xload |= kfXloadOxLegend;
        } else if (strequal_k_unsafe(flagname_p2, "oop-cats")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = CmdlineAllocString(argvk[arg_idx + 1], argvk[arg_idx], kMaxIdSlen, &pc.loop_cats_phenoname);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
        } else if (likely(strequal_k_unsafe(flagname_p2, "d"))) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 2, 4))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          for (uint32_t uii = 0; uii != 2; ++uii) {
            reterr = CmdlineAllocString(argvk[arg_idx + uii + 1], argvk[arg_idx], kMaxIdSlen, &(pc.ld_info.ld_console_varids[uii]));
            if (unlikely(reterr)) {
              goto main_ret_1;
            }
          }
          for (uint32_t param_idx = 3; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            if (!strcmp(cur_modif, "dosage")) {
              pc.ld_info.ld_console_flags |= kfLdConsoleDosage;
            } else if (likely(!strcmp(cur_modif, "hwe-midp"))) {
              pc.ld_info.ld_console_flags |= kfLdConsoleHweMidp;
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --ld parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          pc.command_flags1 |= kfCommand1Ld;
          pc.dependency_flags |= kfFilterAllReq;
        } else if (strequal_k_unsafe(flagname_p2, "oop-assoc")) {
          logerrputs("Error: --loop-assoc is retired.  Use --within + --split-cat-pheno instead.\n");
          goto main_ret_INVALID_CMDLINE_A;
        } else if (strequal_k_unsafe(flagname_p2, "ist-duplicate-vars")) {
          logerrputs("Error: --list-duplicate-vars is retired.  We recommend --set-all-var-ids +\n--rm-dup for variant deduplication.\n");
          goto main_ret_INVALID_CMDLINE_A;
        } else {
          goto main_ret_INVALID_CMDLINE_UNRECOGNIZED;
        }
        break;

      case 'm':
        if (strequal_k_unsafe(flagname_p2, "emory")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          uint32_t mb_modif_idx = 1;
          if (param_ct == 2) {
            if (unlikely(CheckExtraParam(&(argvk[arg_idx]), "require", &mb_modif_idx))) {
              goto main_ret_INVALID_CMDLINE_A;
            }
            memory_require = 1;
          }
          const char* mb_modif = argvk[arg_idx + mb_modif_idx];
          if (unlikely(ScanPosintptrx(mb_modif, R_CAST(uintptr_t*, &malloc_size_mib)))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --memory parameter '%s'.\n", mb_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          if (unlikely(malloc_size_mib < S_CAST(intptr_t, kBigstackMinMib))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --memory parameter '%s' (minimum %u).\n", mb_modif, kBigstackMinMib);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
#ifndef __LP64__
          if (unlikely(malloc_size_mib > S_CAST(intptr_t, kMalloc32bitMibMax))) {
            logerrprintf("Error: --memory parameter too large for 32-bit version (max %u).\n", kMalloc32bitMibMax);
            goto main_ret_INVALID_CMDLINE;
          }
#endif
        } else if (strequal_k_unsafe(flagname_p2, "ake-bed")) {
          if (unlikely(pc.exportf_info.flags & kfExportfIndMajorBed)) {
            logerrputs("Error: --make-bed cannot be used with --export ind-major-bed.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 3))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          for (uint32_t param_idx = 1; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "vzs", cur_modif_slen)) {
              make_plink2_flags |= kfMakeBimZs;
            } else if (strequal_k(cur_modif, "trim-alts", cur_modif_slen)) {
              make_plink2_flags |= kfMakePlink2TrimAlts;
            } else if (strequal_k(cur_modif, "erase-alt2+", cur_modif_slen)) {
              make_plink2_flags |= kfMakePlink2EraseAlt2Plus;
            } else if (likely(
                         StrStartsWith(cur_modif, "m=", cur_modif_slen) ||
                         StrStartsWith(cur_modif, "multiallelics=", cur_modif_slen))) {
              if (unlikely(make_plink2_flags & kfMakePlink2MMask)) {
                logerrputs("Error: Multiple --make-bed multiallelics= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              const char* mode_start = (cur_modif[1] == '=')? (&(cur_modif[strlen("m=")])) : (&(cur_modif[strlen("multiallelics=")]));
              const uint32_t mode_slen = cur_modif_slen - S_CAST(uintptr_t, mode_start - cur_modif);
              if (strequal_k(mode_start, "-", mode_slen)) {
                make_plink2_flags |= kfMakePlink2MSplitAll;
              } else if (strequal_k(mode_start, "-snps", mode_slen)) {
                make_plink2_flags |= kfMakePlink2MSplitSnps;
              } else {
                // merge modes not supported here
                snprintf(g_logbuf, kLogbufSize, "Error: Invalid --make-bed multiallelics= split mode '%s'.\n", mode_start);
                goto main_ret_INVALID_CMDLINE_WWA;
              }
            } else {
              char* write_iter = strcpya_k(g_logbuf, "Error: Invalid --make-bed parameter '");
              write_iter = memcpya(write_iter, cur_modif, cur_modif_slen);
              write_iter = strcpya_k(write_iter, "'.");
              if ((param_idx == 1) && (!outname_end)) {
                // the missing --out mistake is so common--I must have made it
                // over a hundred times by now--that a custom error message is
                // worthwhile.
                write_iter = strcpya_k(write_iter, " (Did you forget '--out'?)");
              }
              *write_iter++ = '\n';
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          if ((make_plink2_flags & (kfMakePlink2TrimAlts | kfMakePlink2EraseAlt2Plus)) == (kfMakePlink2TrimAlts | kfMakePlink2EraseAlt2Plus)) {
            // Prohibit this combination since it's really only meaningful if
            // alt1 is sometimes absent when a higher-numbered alt is not, and
            // the erasure would only apply to the *post*-trimming alt2+
            // alleles; but that's confusing, and already easy enough to
            // achieve by splitting up the operation.
            //
            // todo: think about whether any multiallelics= interactions are
            // worth prohibiting
            logerrputs("Error: --make-bed 'trim-alts' and 'erase-alt2+' modifiers cannot be used\ntogether.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          make_plink2_flags |= kfMakeBed | kfMakeBim | kfMakeFam;
          pc.command_flags1 |= kfCommand1MakePlink2;
          pc.dependency_flags |= kfFilterAllReq;
        } else if (strequal_k_unsafe(flagname_p2, "ake-bpgen")) {
          if (unlikely(make_plink2_flags & kfMakeBed)) {
            logerrputs("Error: --make-bpgen cannot be used with --make-bed.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(import_flags & kfImportKeepAutoconv)) {
            logerrputs("Error: --make-bpgen cannot be used with --keep-autoconv.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 7))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          uint32_t vid_semicolon = 0;
          for (uint32_t param_idx = 1; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "vzs", cur_modif_slen)) {
              make_plink2_flags |= kfMakeBimZs;
            } else if (StrStartsWith(cur_modif, "format=", cur_modif_slen)) {
              if (unlikely(make_plink2_flags & (kfMakePgenFormatBase * 3))) {
                logerrputs("Error: Multiple --make-bpgen format= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              const uint32_t fcode_minus_2 = ctou32(cur_modif[7]) - 50;
              if (unlikely((fcode_minus_2 > 2) || cur_modif[8])) {
                snprintf(g_logbuf, kLogbufSize, "Error: Invalid --make-bpgen format code '%s'.\n", &(cur_modif[7]));
                goto main_ret_INVALID_CMDLINE_WWA;
              }
              if (fcode_minus_2) {
                logerrputs("Error: --make-bpgen formats 3 and 4 (unphased/phased dosage) are not\nimplemented yet.\n");
                reterr = kPglRetNotYetSupported;
                goto main_ret_1;
              }
              make_plink2_flags = S_CAST(MakePlink2Flags, make_plink2_flags | (kfMakePgenFormatBase * (1 + fcode_minus_2)));
            } else if (StrStartsWith(cur_modif, "m=", cur_modif_slen) ||
                       StrStartsWith(cur_modif, "multiallelics=", cur_modif_slen)) {
              if (unlikely(make_plink2_flags & kfMakePlink2MMask)) {
                logerrputs("Error: Multiple --make-bpgen multiallelics= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              // er, some of this belongs in its own function...
              const char* mode_start = (cur_modif[1] == '=')? (&(cur_modif[strlen("m=")])) : (&(cur_modif[strlen("multiallelics=")]));
              const uint32_t mode_slen = cur_modif_slen - S_CAST(uintptr_t, mode_start - cur_modif);
              if (strequal_k(mode_start, "-", mode_slen)) {
                make_plink2_flags |= kfMakePlink2MSplitAll;
              } else if (strequal_k(mode_start, "-snps", mode_slen)) {
                make_plink2_flags |= kfMakePlink2MSplitSnps;
              } else if (strequal_k(mode_start, "+", mode_slen) ||
                         strequal_k(mode_start, "+both", mode_slen)) {
                make_plink2_flags |= kfMakePlink2MJoinBoth;
              } else if (strequal_k(mode_start, "+snps", mode_slen)) {
                make_plink2_flags |= kfMakePlink2MJoinSnps;
              } else if (likely(strequal_k(mode_start, "+any", mode_slen))) {
                make_plink2_flags |= kfMakePlink2MJoinAny;
              } else {
                snprintf(g_logbuf, kLogbufSize, "Error: Invalid --make-bpgen multiallelics= mode '%s'.\n", mode_start);
                goto main_ret_INVALID_CMDLINE_WWA;
              }
            } else if (strequal_k(cur_modif, "trim-alts", cur_modif_slen)) {
              make_plink2_flags |= kfMakePlink2TrimAlts;
            } else if (strequal_k(cur_modif, "erase-alt2+", cur_modif_slen)) {
              make_plink2_flags |= kfMakePlink2EraseAlt2Plus;
            } else if (strequal_k(cur_modif, "vid-split", cur_modif_slen)) {
              vid_semicolon |= 1;
            } else if (strequal_k(cur_modif, "vid-split-dup", cur_modif_slen)) {
              vid_semicolon |= 2;
            } else if (strequal_k(cur_modif, "vid-join", cur_modif_slen)) {
              vid_semicolon |= 4;
            } else if (strequal_k(cur_modif, "vid-dup", cur_modif_slen)) {
              make_plink2_flags |= kfMakePlink2VidDup;
            } else if (strequal_k(cur_modif, "erase-phase", cur_modif_slen)) {
              make_plink2_flags |= kfMakePgenErasePhase;
            } else if (likely(strequal_k(cur_modif, "erase-dosage", cur_modif_slen))) {
              make_plink2_flags |= kfMakePgenEraseDosage;
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --make-bpgen parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          if ((make_plink2_flags & (kfMakePlink2TrimAlts | kfMakePlink2EraseAlt2Plus)) == (kfMakePlink2TrimAlts | kfMakePlink2EraseAlt2Plus)) {
            logerrputs("Error: --make-bpgen 'trim-alts' and 'erase-alt2+' modifiers cannot be used\ntogether.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (vid_semicolon) {
            if (unlikely((make_plink2_flags & kfMakePlink2VidDup) || (vid_semicolon & (vid_semicolon - 1)))) {
              logerrputs("Error: --make-bpgen 'vid-split', 'vid-split-dup', 'vid-dup', and 'vid-join'\nmodifiers are mutually exclusive.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
            if (vid_semicolon & 3) {
              if (unlikely((make_plink2_flags & kfMakePlink2MJoin) || (!(make_plink2_flags & kfMakePlink2MMask)))) {
                logerrputs("Error: --make-bpgen 'vid-split' must be used with a multiallelics= split mode.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
            } else {
              if (unlikely(!(make_plink2_flags & kfMakePlink2MJoin))) {
                logerrputs("Error: --make-bpgen 'vid-join' must be used with a multiallelics= join mode.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
            }
            make_plink2_flags |= kfMakePlink2VidSemicolon;
            if (vid_semicolon == 2) {
              make_plink2_flags |= kfMakePlink2VidDup;
            }
          } else if (make_plink2_flags & kfMakePlink2VidDup) {
            if (unlikely((make_plink2_flags & kfMakePlink2MJoin) || (!(make_plink2_flags & kfMakePlink2MMask)))) {
              logerrputs("Error: --make-bpgen 'vid-dup' must be used with a multiallelics= split mode.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
          }
          make_plink2_flags |= kfMakePgen | kfMakeBim | kfMakeFam;
          pc.command_flags1 |= kfCommand1MakePlink2;
          pc.dependency_flags |= kfFilterAllReq;
        } else if (strequal_k_unsafe(flagname_p2, "ake-pgen")) {
          if (unlikely(make_plink2_flags & (kfMakeBed | kfMakePgen))) {
            logerrputs("Error: --make-pgen cannot be used with --make-bed/--make-bpgen.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(make_plink2_flags & (kfMakeBim | kfMakeFam | kfMakePvar | kfMakePsam))) {
            logerrputs("Error: --make-just-... cannot be used with --make-bed/--make-[b]pgen.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(import_flags & kfImportKeepAutoconv)) {
            logerrputs("Error: --make-pgen cannot be used with --keep-autoconv.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 9))) {
            goto main_ret_INVALID_CMDLINE_A;
          }
          uint32_t explicit_pvar_cols = 0;
          uint32_t explicit_psam_cols = 0;
          uint32_t vid_semicolon = 0;
          for (uint32_t param_idx = 1; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "vzs", cur_modif_slen)) {
              pc.pvar_psam_flags |= kfPvarZs;
            } else if (StrStartsWith0(cur_modif, "pvar-cols=", cur_modif_slen)) {
              if (unlikely(explicit_pvar_cols)) {
                logerrputs("Error: Multiple --make-pgen pvar-cols= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              explicit_pvar_cols = 1;
              reterr = ParseColDescriptor(&(cur_modif[strlen("pvar-cols=")]), "xheader\0maybequal\0qual\0maybefilter\0filter\0maybeinfo\0info\0maybecm\0cm\0", "--make-pgen pvar-cols", kfPvarColXheader, kfPvarColDefault, 0, &pc.pvar_psam_flags);
              if (unlikely(reterr)) {
                goto main_ret_1;
              }
              if (unlikely((pc.pvar_psam_flags & kfPvarColXinfo) && (!(pc.pvar_psam_flags & kfPvarColXheader)))) {
                logerrputs("Error: --make-pgen pvar-cols= expression cannot exclude xheader when info is\npresent.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
            } else if (StrStartsWith(cur_modif, "format=", cur_modif_slen)) {
              if (unlikely(make_plink2_flags & (kfMakePgenFormatBase * 3))) {
                logerrputs("Error: Multiple --make-pgen format= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              const uint32_t fcode_minus_2 = ctou32(cur_modif[7]) - 50;
              if (unlikely((fcode_minus_2 > 2) || cur_modif[8])) {
                snprintf(g_logbuf, kLogbufSize, "Error: Invalid --make-pgen format code '%s'.\n", &(cur_modif[7]));
                goto main_ret_INVALID_CMDLINE_WWA;
              }
              if (fcode_minus_2) {
                logerrputs("Error: --make-pgen formats 3 and 4 (unphased/phased dosage) are not implemented\nyet.\n");
                reterr = kPglRetNotYetSupported;
                goto main_ret_1;
              }
              make_plink2_flags = S_CAST(MakePlink2Flags, make_plink2_flags | (kfMakePgenFormatBase * (1 + fcode_minus_2)));
            } else if (StrStartsWith(cur_modif, "m=", cur_modif_slen) ||
                       StrStartsWith(cur_modif, "multiallelics=", cur_modif_slen)) {
              if (unlikely(make_plink2_flags & kfMakePlink2MMask)) {
                logerrputs("Error: Multiple --make-pgen multiallelics= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              const char* mode_start = (cur_modif[1] == '=')? (&(cur_modif[2])) : (&(cur_modif[14]));
              const uint32_t mode_slen = cur_modif_slen - S_CAST(uintptr_t, mode_start - cur_modif);
              if (strequal_k(mode_start, "-", mode_slen)) {
                make_plink2_flags |= kfMakePlink2MSplitAll;
              } else if (strequal_k(mode_start, "-snps", mode_slen)) {
                make_plink2_flags |= kfMakePlink2MSplitSnps;
              } else if (strequal_k(mode_start, "+", mode_slen) ||
                         strequal_k(mode_start, "+both", mode_slen)) {
                make_plink2_flags |= kfMakePlink2MJoinBoth;
              } else if (strequal_k(mode_start, "+snps", mode_slen)) {
                make_plink2_flags |= kfMakePlink2MJoinSnps;
              } else if (likely(strequal_k(mode_start, "+any", mode_slen))) {
                make_plink2_flags |= kfMakePlink2MJoinAny;
              } else {
                snprintf(g_logbuf, kLogbufSize, "Error: Invalid --make-pgen multiallelics= mode '%s'.\n", mode_start);
                goto main_ret_INVALID_CMDLINE_WWA;
              }
            } else if (strequal_k(cur_modif, "trim-alts", cur_modif_slen)) {
              make_plink2_flags |= kfMakePlink2TrimAlts;
            } else if (strequal_k(cur_modif, "erase-alt2+", cur_modif_slen)) {
              make_plink2_flags |= kfMakePlink2EraseAlt2Plus;
            } else if (strequal_k(cur_modif, "vid-split", cur_modif_slen)) {
              vid_semicolon |= 1;
            } else if (strequal_k(cur_modif, "vid-split-dup", cur_modif_slen)) {
              vid_semicolon |= 2;
            } else if (strequal_k(cur_modif, "vid-join", cur_modif_slen)) {
              vid_semicolon |= 4;
            } else if (strequal_k(cur_modif, "vid-dup", cur_modif_slen)) {
              make_plink2_flags |= kfMakePlink2VidDup;
            } else if (strequal_k(cur_modif, "erase-phase", cur_modif_slen)) {
              make_plink2_flags |= kfMakePgenErasePhase;
            } else if (strequal_k(cur_modif, "erase-dosage", cur_modif_slen)) {
              make_plink2_flags |= kfMakePgenEraseDosage;
            } else if (likely(StrStartsWith0(cur_modif, "psam-cols=", cur_modif_slen))) {
              if (unlikely(explicit_psam_cols)) {
                logerrputs("Error: Multiple --make-pgen psam-cols= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              explicit_psam_cols = 1;
              reterr = ParseColDescriptor(&(cur_modif[strlen("psam-cols=")]), "maybefid\0fid\0maybesid\0sid\0maybeparents\0parents\0sex\0pheno1\0phenos\0", "make-pgen psam-cols", kfPsamColMaybefid, kfPsamColDefault, 0, &pc.pvar_psam_flags);
              if (unlikely(reterr)) {
                goto main_ret_1;
              }
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --make-pgen parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          if ((make_plink2_flags & (kfMakePlink2TrimAlts | kfMakePlink2EraseAlt2Plus)) == (kfMakePlink2TrimAlts | kfMakePlink2EraseAlt2Plus)) {
            logerrputs("Error: --make-pgen 'trim-alts' and 'erase-alt2+' modifiers cannot be used\ntogether.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (vid_semicolon) {
            if (unlikely((make_plink2_flags & kfMakePlink2VidDup) || (vid_semicolon & (vid_semicolon - 1)))) {
              logerrputs("Error: --make-pgen 'vid-split', 'vid-split-dup', 'vid-dup', and 'vid-join'\nmodifiers are mutually exclusive.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
            if (vid_semicolon & 3) {
              if (unlikely((make_plink2_flags & kfMakePlink2MJoin) || (!(make_plink2_flags & kfMakePlink2MMask)))) {
                logerrputs("Error: --make-pgen 'vid-split' must be used with a multiallelics= split mode.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
            } else {
              if (unlikely(!(make_plink2_flags & kfMakePlink2MJoin))) {
                logerrputs("Error: --make-pgen 'vid-join' must be used with a multiallelics= join mode.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
            }
            make_plink2_flags |= kfMakePlink2VidSemicolon;
            if (vid_semicolon == 2) {
              make_plink2_flags |= kfMakePlink2VidDup;
            }
          }
          if (!explicit_pvar_cols) {
            pc.pvar_psam_flags |= kfPvarColDefault;
          }
          if (!explicit_psam_cols) {
            pc.pvar_psam_flags |= kfPsamColDefault;
          }
          make_plink2_flags |= kfMakePgen | kfMakePvar | kfMakePsam;
          pc.command_flags1 |= kfCommand1MakePlink2;
          pc.dependency_flags |= kfFilterAllReq;
        } else if (strequal_k_unsafe(flagname_p2, "ake-just-bim")) {
          if (unlikely(make_plink2_flags & (kfMakeBed | kfMakePgen))) {
            logerrputs("Error: --make-just-... cannot be used with --make-bed/--make-[b]pgen.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (param_ct) {
            const char* cur_modif = argvk[arg_idx + 1];
            if (likely(!strcmp(cur_modif, "zs"))) {
              make_plink2_flags |= kfMakeBimZs;
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --make-just-bim parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          make_plink2_flags |= kfMakeBim;
          pc.command_flags1 |= kfCommand1MakePlink2;
          pc.dependency_flags |= kfFilterPvarReq;
        } else if (strequal_k_unsafe(flagname_p2, "ake-just-fam")) {
          if (unlikely(make_plink2_flags & (kfMakeBed | kfMakePgen))) {
            logerrputs("Error: --make-just-... cannot be used with --make-bed/--make-[b]pgen.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          make_plink2_flags |= kfMakeFam;
          pc.command_flags1 |= kfCommand1MakePlink2;
          pc.dependency_flags |= kfFilterPsamReq;
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "ake-just-pvar")) {
          if (unlikely(make_plink2_flags & (kfMakeBed | kfMakePgen))) {
            logerrputs("Error: --make-just-... cannot be used with --make-bed/--make-[b]pgen.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          uint32_t explicit_cols = 0;
          for (uint32_t param_idx = 1; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "zs", cur_modif_slen)) {
              pc.pvar_psam_flags |= kfPvarZs;
            } else if (likely(StrStartsWith0(cur_modif, "cols=", cur_modif_slen))) {
              if (unlikely(explicit_cols)) {
                logerrputs("Error: Multiple --make-just-pvar cols= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              explicit_cols = 1;
              reterr = ParseColDescriptor(&(cur_modif[5]), "xheader\0maybequal\0qual\0maybefilter\0filter\0maybeinfo\0info\0maybecm\0cm\0", "make-just-pvar", kfPvarColXheader, kfPvarColDefault, 0, &pc.pvar_psam_flags);
              if (unlikely(reterr)) {
                goto main_ret_1;
              }
              if (unlikely((pc.pvar_psam_flags & kfPvarColXinfo) && (!(pc.pvar_psam_flags & kfPvarColXheader)))) {
                logerrputs("Error: --make-just-pvar cols= expression cannot exclude xheader when info is\npresent.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              if (pc.pvar_psam_flags & kfPvarColInfo) {
                pc.dependency_flags |= kfFilterNonrefFlagsNeededSet;
              }
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --make-just-pvar parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          if (!explicit_cols) {
            pc.pvar_psam_flags |= kfPvarColDefault;
          }
          make_plink2_flags |= kfMakePvar;
          pc.command_flags1 |= kfCommand1MakePlink2;
          pc.dependency_flags |= kfFilterPvarReq;
        } else if (strequal_k_unsafe(flagname_p2, "ake-just-psam")) {
          if (unlikely(make_plink2_flags & (kfMakeBed | kfMakePgen))) {
            logerrputs("Error: --make-just-... cannot be used with --make-bed/--make-[b]pgen.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (param_ct) {
            const char* cur_modif = argvk[arg_idx + 1];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (likely(StrStartsWith0(cur_modif, "cols=", cur_modif_slen))) {
              reterr = ParseColDescriptor(&(cur_modif[5]), "maybefid\0fid\0maybesid\0sid\0maybeparents\0parents\0sex\0pheno1\0phenos\0", "make-just-psam", kfPsamColMaybefid, kfPsamColDefault, 0, &pc.pvar_psam_flags);
              if (unlikely(reterr)) {
                goto main_ret_1;
              }
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --make-just-psam parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          } else {
            pc.pvar_psam_flags |= kfPsamColDefault;
          }
          make_plink2_flags |= kfMakePsam;
          pc.command_flags1 |= kfCommand1MakePlink2;
          pc.dependency_flags |= kfFilterPsamReq;
        } else if (strequal_k_unsafe(flagname_p2, "ake-king")) {
          // may want to add options for handling X/Y/MT
          if (unlikely(king_cutoff_fprefix)) {
            logerrputs("Error: --make-king cannot be used with a --king-cutoff input fileset.\n");
            goto main_ret_INVALID_CMDLINE_A;
          } else if (unlikely(pc.king_table_subset_fname)) {
            logerrputs("Error: --make-king cannot be used with --king-table-subset.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          for (uint32_t param_idx = 1; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "zs", cur_modif_slen)) {
              if (unlikely(pc.king_flags & kfKingMatrixEncodemask)) {
                logerrputs("Error: Multiple --make-king encoding modifiers.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              pc.king_flags |= kfKingMatrixZs;
            } else if (strequal_k(cur_modif, "bin", cur_modif_slen)) {
              if (unlikely(pc.king_flags & kfKingMatrixEncodemask)) {
                logerrputs("Error: Multiple --make-king encoding modifiers.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              pc.king_flags |= kfKingMatrixBin;
            } else if (strequal_k(cur_modif, "bin4", cur_modif_slen)) {
              if (unlikely(pc.king_flags & kfKingMatrixEncodemask)) {
                logerrputs("Error: Multiple --make-king encoding modifiers.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              pc.king_flags |= kfKingMatrixBin4;
            } else if (strequal_k(cur_modif, "square", cur_modif_slen)) {
              if (unlikely(pc.king_flags & kfKingMatrixShapemask)) {
                logerrputs("Error: Multiple --make-king shape modifiers.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              pc.king_flags |= kfKingMatrixSq;
            } else if (strequal_k(cur_modif, "square0", cur_modif_slen)) {
              if (unlikely(pc.king_flags & kfKingMatrixShapemask)) {
                logerrputs("Error: Multiple --make-king shape modifiers.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              pc.king_flags |= kfKingMatrixSq0;
            } else if (likely(strequal_k(cur_modif, "triangle", cur_modif_slen))) {
              if (unlikely(pc.king_flags & kfKingMatrixShapemask)) {
                logerrputs("Error: Multiple --make-king shape modifiers.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              pc.king_flags |= kfKingMatrixTri;
            } else if (strequal_k(cur_modif, "no-idheader", cur_modif_slen)) {
              logerrputs("Error: --make-king 'no-idheader' modifier retired.  Use --no-id-header instead.\n");
              goto main_ret_INVALID_CMDLINE_A;
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --make-king parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          if (!(pc.king_flags & kfKingMatrixShapemask)) {
            if (pc.king_flags & (kfKingMatrixBin | kfKingMatrixBin4)) {
              pc.king_flags |= kfKingMatrixSq;
            } else {
              pc.king_flags |= kfKingMatrixTri;
            }
          }
          pc.command_flags1 |= kfCommand1MakeKing;
          pc.dependency_flags |= kfFilterAllReq;
        } else if (strequal_k_unsafe(flagname_p2, "ake-king-table")) {
          if (unlikely(king_cutoff_fprefix)) {
            logerrputs("Error: --make-king-table cannot be used with a --king-cutoff input fileset.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 4))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          for (uint32_t param_idx = 1; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "zs", cur_modif_slen)) {
              pc.king_flags |= kfKingTableZs;
            } else if (strequal_k(cur_modif, "counts", cur_modif_slen)) {
              pc.king_flags |= kfKingCounts;
            } else if (strequal_k(cur_modif, "rel-check", cur_modif_slen)) {
              if (unlikely(pc.king_cutoff != -1)) {
                logerrputs("Error: --make-king-table 'rel-check' modifier cannot be used with\n--king-cutoff.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              pc.king_flags |= kfKingRelCheck;
            } else if (likely(StrStartsWith(cur_modif, "cols=", cur_modif_slen))) {
              if (unlikely(pc.king_flags & kfKingColAll)) {
                logerrputs("Error: Multiple --make-king-table cols= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              reterr = ParseColDescriptor(&(cur_modif[5]), "maybefid\0fid\0id\0maybesid\0sid\0nsnp\0hethet\0ibs0\0ibs1\0kinship\0", "make-king-table", kfKingColMaybefid, kfKingColDefault, 1, &pc.king_flags);
              if (unlikely(reterr)) {
                goto main_ret_1;
              }
              if (!(pc.king_flags & kfKingColId)) {
                if (unlikely(pc.king_flags & (kfKingColMaybefid | kfKingColFid | kfKingColMaybesid | kfKingColSid))) {
                  logerrputs("Error: Invalid --make-king-table column set descriptor ('maybefid', 'fid',\n'maybesid', and 'sid' require 'id').\n");
                  goto main_ret_INVALID_CMDLINE_A;
                }
                if (unlikely(pc.king_table_filter != -DBL_MAX)) {
                  logerrputs("Error: --king-table-filter requires --make-king-table cols= to include the 'id'\ncolumn set.\n");
                  goto main_ret_INVALID_CMDLINE_A;
                }
                if (unlikely(pc.king_table_subset_fname)) {
                  logerrputs("Error: --king-table-subset requires --make-king-table cols= to include the 'id'\ncolumn set.\n");
                  goto main_ret_INVALID_CMDLINE_A;
                }
              }
            } else if (strequal_k(cur_modif, "no-idheader", cur_modif_slen)) {
              logerrputs("Error: --make-king-table 'no-idheader' modifier retired.  Use --no-id-header\ninstead.\n");
              goto main_ret_INVALID_CMDLINE_A;
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --make-king-table parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          if (!(pc.king_flags & kfKingColAll)) {
            pc.king_flags |= kfKingColDefault;
          }
          pc.command_flags1 |= kfCommand1MakeKing;
          pc.dependency_flags |= kfFilterAllReq;
        } else if (strequal_k_unsafe(flagname_p2, "issing")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 4))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          for (uint32_t param_idx = 1; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "zs", cur_modif_slen)) {
              pc.missing_rpt_flags |= kfMissingRptZs;
            } else if (strequal_k(cur_modif, "sample-only", cur_modif_slen)) {
              if (unlikely(pc.missing_rpt_flags & kfMissingRptVariantOnly)) {
                logerrputs("Error: --missing 'sample-only' and 'variant-only' cannot be used together.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              pc.missing_rpt_flags |= kfMissingRptSampleOnly;
            } else if (strequal_k(cur_modif, "variant-only", cur_modif_slen)) {
              if (unlikely(pc.missing_rpt_flags & kfMissingRptSampleOnly)) {
                logerrputs("Error: --missing 'sample-only' and 'variant-only' cannot be used together.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              pc.missing_rpt_flags |= kfMissingRptVariantOnly;
            } else if (StrStartsWith(cur_modif, "scols=", cur_modif_slen)) {
              if (unlikely(pc.missing_rpt_flags & kfMissingRptScolAll)) {
                logerrputs("Error: Multiple --missing scols= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              reterr = ParseColDescriptor(&(cur_modif[strlen("scols=")]), "maybefid\0fid\0maybesid\0sid\0misspheno1\0missphenos\0nmissdosage\0nmiss\0nmisshh\0hethap\0nobs\0fmissdosage\0fmiss\0fmisshh\0", "missing scols", kfMissingRptScolMaybefid, kfMissingRptScolDefault, 1, &pc.missing_rpt_flags);
              if (unlikely(reterr)) {
                goto main_ret_1;
              }
            } else if (likely(StrStartsWith(cur_modif, "vcols=", cur_modif_slen))) {
              if (unlikely(pc.missing_rpt_flags & kfMissingRptVcolAll)) {
                logerrputs("Error: Multiple --missing vcols= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              reterr = ParseColDescriptor(&(cur_modif[strlen("vcols=")]), "chrom\0pos\0ref\0alt1\0alt\0nmissdosage\0nmiss\0nmisshh\0hethap\0nobs\0fmissdosage\0fmiss\0fmisshh\0fhethap\0", "missing vcols", kfMissingRptVcolChrom, kfMissingRptVcolDefault, 1, &pc.missing_rpt_flags);
              if (unlikely(reterr)) {
                goto main_ret_1;
              }
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --missing parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          const uint32_t explicit_scols = pc.missing_rpt_flags & kfMissingRptScolAll;
          if (pc.missing_rpt_flags & kfMissingRptVariantOnly) {
            if (unlikely(explicit_scols)) {
              logerrputs("Error: --missing 'variant-only' and 'scols=' modifiers cannot be used together.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
          } else {
            if (!explicit_scols) {
              pc.missing_rpt_flags |= kfMissingRptScolDefault;
            }
          }
          const uint32_t explicit_vcols = pc.missing_rpt_flags & kfMissingRptVcolAll;
          if (pc.missing_rpt_flags & kfMissingRptSampleOnly) {
            if (unlikely(explicit_vcols)) {
              logerrputs("Error: --missing 'sample-only' and 'vcols=' modifiers cannot be used together.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
          } else if (!explicit_vcols) {
            pc.missing_rpt_flags |= kfMissingRptVcolDefault;
          }
          pc.command_flags1 |= kfCommand1MissingReport;
          pc.dependency_flags |= kfFilterAllReq;
        } else if (strequal_k_unsafe(flagname_p2, "aj-ref")) {
          if (unlikely(pc.alt1_allele_flag)) {
            logerrputs("Error: --maj-ref cannot be used with --ref-allele/--alt1-allele.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (param_ct) {
            const char* cur_modif = argvk[arg_idx + 1];
            if (likely(!strcmp(cur_modif, "force"))) {
              pc.misc_flags |= kfMiscMajRefForce;
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --maj-ref parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          pc.misc_flags |= kfMiscMajRef;
          pc.dependency_flags |= kfFilterAllReq | kfFilterNoSplitChr;
        } else if (strequal_k_unsafe(flagname_p2, "af")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (param_ct) {
            const char* cur_modif = argvk[arg_idx + 1];
            const char* mode_str = ScanadvDouble(cur_modif, &pc.min_maf);
            if (!mode_str) {
              pc.min_maf = 0.01;
              if (unlikely(param_ct == 2)) {
                logerrputs("Error: Invalid --maf parameter sequence.\n");
                goto main_ret_INVALID_CMDLINE_WWA;
              }
              mode_str = cur_modif;
            } else {
              if (unlikely(pc.min_maf < 0.0)) {
                snprintf(g_logbuf, kLogbufSize, "Error: --maf parameter '%s' too small (must be >= 0).\n", cur_modif);
                goto main_ret_INVALID_CMDLINE_WWA;
              } else if (unlikely(pc.min_maf > 1.0)) {
                snprintf(g_logbuf, kLogbufSize, "Error: --maf parameter '%s' too large (must be <= 1).\n", cur_modif);
                goto main_ret_INVALID_CMDLINE_WWA;
              }
              if (mode_str[0] == ':') {
                if (unlikely(param_ct == 2)) {
                  logerrputs("Error: Invalid --maf parameter sequence.\n");
                  goto main_ret_INVALID_CMDLINE_WWA;
                }
              } else {
                if (unlikely(mode_str[0])) {
                  snprintf(g_logbuf, kLogbufSize, "Error: Invalid --maf parameter '%s'.\n", cur_modif);
                  goto main_ret_INVALID_CMDLINE_WWA;
                }
                if (param_ct == 2) {
                  mode_str = argvk[arg_idx + 2];
                } else {
                  mode_str = nullptr;
                }
              }
            }
            if (mode_str) {
              if (ParseFreqSelector(mode_str, flagname_p, &(pc.filter_modes[0]))) {
                goto main_ret_INVALID_CMDLINE_WWA;
              }
            }
          } else {
            pc.min_maf = 0.01;
          }
          if (pc.min_maf != 0.0) {
            pc.filter_flags |= kfFilterPvarReq;
            pc.dependency_flags |= kfFilterAllReq | kfFilterNoSplitChr;
          }
        } else if (strequal_k_unsafe(flagname_p2, "ax-maf")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          const char* mode_str = ScanadvDouble(cur_modif, &pc.max_maf);
          if (unlikely(!mode_str)) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --max-maf parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          if (unlikely(pc.max_maf >= 1.0)) {
            snprintf(g_logbuf, kLogbufSize, "Error: --max-maf parameter '%s' too large (must be < 1).\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          if (mode_str[0] == ':') {
            if (unlikely(param_ct == 2)) {
              logerrputs("Error: Invalid --max-maf parameter sequence.\n");
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          } else {
            if (unlikely(mode_str[0])) {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --max-maf parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
            if (param_ct == 2) {
              mode_str = argvk[arg_idx + 2];
            } else {
              mode_str = nullptr;
            }
          }
          if (mode_str) {
            if (ParseFreqSelector(mode_str, flagname_p, &(pc.filter_modes[1]))) {
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          if (unlikely((pc.filter_modes[0] == pc.filter_modes[1]) && (pc.max_maf < pc.min_maf))) {
            snprintf(g_logbuf, kLogbufSize, "Error: --max-maf parameter '%s' too small (must be >= %g).\n", cur_modif, pc.min_maf);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          pc.filter_flags |= kfFilterPvarReq;
          pc.dependency_flags |= kfFilterAllReq | kfFilterNoSplitChr;
        } else if (strequal_k_unsafe(flagname_p2, "ac")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          double dxx;
          const char* mode_str = ScanadvDouble(cur_modif, &dxx);
          if (unlikely((!mode_str) || (dxx < 0.0) || (dxx > 2147483646.0))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --mac parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE;
          }
          if (dxx > 0.0) {
            // round up, but keep as much precision as possible
            int32_t int_part = S_CAST(int32_t, dxx);
            dxx -= int_part;
            pc.min_allele_dosage = int_part * S_CAST(uint64_t, kDosageMax);
            if (dxx > 0.0) {
              pc.min_allele_dosage += 1 + S_CAST(uint64_t, dxx * (kDosageMax * (1 - kSmallEpsilon)));
            }
            // yeah, this should be its own function...
            if (mode_str[0] == ':') {
              if (unlikely(param_ct == 2)) {
                logerrputs("Error: Invalid --mac parameter sequence.\n");
                goto main_ret_INVALID_CMDLINE_WWA;
              }
            } else {
              if (unlikely(mode_str[0])) {
                snprintf(g_logbuf, kLogbufSize, "Error: Invalid --mac parameter '%s'.\n", cur_modif);
                goto main_ret_INVALID_CMDLINE_WWA;
              }
              if (param_ct == 2) {
                mode_str = argvk[arg_idx + 2];
              } else {
                mode_str = nullptr;
              }
            }
            if (mode_str) {
              if (ParseFreqSelector(mode_str, flagname_p, &(pc.filter_modes[2]))) {
                goto main_ret_INVALID_CMDLINE_WWA;
              }
            }
            pc.filter_flags |= kfFilterPvarReq;
            pc.dependency_flags |= kfFilterAllReq | kfFilterNoSplitChr;
          }
        } else if (strequal_k_unsafe(flagname_p2, "ax-mac")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          double dxx;
          const char* mode_str = ScanadvDouble(cur_modif, &dxx);
          if (unlikely((!mode_str) || (dxx < 0.0) || (dxx > 2147483646.0))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --max-mac parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          // round down
          pc.max_allele_dosage = S_CAST(int64_t, dxx * kDosageMax);
          if (mode_str[0] == ':') {
            if (unlikely(param_ct == 2)) {
              logerrputs("Error: Invalid --max-mac parameter sequence.\n");
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          } else {
            if (unlikely(mode_str[0])) {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --max-mac parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
            if (param_ct == 2) {
              mode_str = argvk[arg_idx + 2];
            } else {
              mode_str = nullptr;
            }
          }
          if (mode_str) {
            if (ParseFreqSelector(mode_str, flagname_p, &(pc.filter_modes[3]))) {
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          if (unlikely((pc.filter_modes[2] == pc.filter_modes[3]) && (pc.max_allele_dosage < pc.min_allele_dosage))) {
            // yeah, --mac 0.1 --max-mac 0.1 also isn't allowed
            logerrputs("Error: --max-mac parameter cannot be smaller than --mac parameter when modes\nare identical.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          pc.filter_flags |= kfFilterPvarReq;
          pc.dependency_flags |= kfFilterAllReq | kfFilterNoSplitChr;
        } else if (strequal_k_unsafe(flagname_p2, "ind")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          uint32_t mind_thresh_present = 0;
          for (uint32_t param_idx = 1; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            if (!strcmp(cur_modif, "dosage")) {
              pc.misc_flags |= kfMiscMindDosage;
            } else if (!strcmp(cur_modif, "hh-missing")) {
              pc.misc_flags |= kfMiscMindHhMissing;
            } else if (unlikely(mind_thresh_present)) {
              logerrputs("Error: Invalid --mind parameter sequence.\n");
              goto main_ret_INVALID_CMDLINE_A;
            } else if (unlikely(!ScantokDouble(cur_modif, &pc.mind_thresh))) {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --mind parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            } else if (unlikely((pc.mind_thresh < 0.0) || (pc.mind_thresh > 1.0))) {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --mind parameter '%s' (must be in [0, 1]).\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            } else {
              mind_thresh_present = 1;
            }
          }
          if (!mind_thresh_present) {
            pc.mind_thresh = 0.1;
          }
          if (pc.mind_thresh < 1.0) {
            pc.filter_flags |= kfFilterPsamReq;
            pc.dependency_flags |= kfFilterAllReq | kfFilterNoSplitChr;
          }
        } else if (strequal_k_unsafe(flagname_p2, "issing-var-code")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = CmdlineAllocString(argvk[arg_idx + 1], argvk[arg_idx], kMaxIdSlen, &pc.missing_varid_match);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
        } else if (strequal_k_unsafe(flagname_p2, "erge-par")) {
          if (pc.exportf_info.flags & kfExportfVcf) {
            logerrputs("Warning: --merge-par should not be used with VCF export.  (The VCF export\nroutine automatically converts PAR1/PAR2 chromosome codes to X, while using\nthe PAR boundaries to get male ploidy right; --merge-par causes VCF export to\nget male ploidy wrong.)\n");
          }
          pc.misc_flags |= kfMiscMergePar;
          pc.dependency_flags |= kfFilterPvarReq | kfFilterNoSplitChr;
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "erge-x")) {
          if (unlikely(pc.misc_flags & kfMiscMergePar)) {
            logerrputs("Error: --merge-par cannot be used with --merge-x.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (pc.exportf_info.flags & kfExportfVcf) {
            logerrputs("Warning: --merge-x should not be used in the same run as VCF export; this\ncauses some ploidies to be wrong.  Instead, use --merge-x + --sort-vars +\n--make-[b]pgen in one run, and follow up with --split-par + --export vcf.\n");
          }
          pc.misc_flags |= kfMiscMergeX;
          pc.dependency_flags |= kfFilterPvarReq;
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "af-succ")) {
          pc.misc_flags |= kfMiscMafSucc;
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "ax-corr")) {
          if (unlikely(!(pc.command_flags1 & kfCommand1Glm))) {
            logerrputs("Error: --max-corr must be used with --glm.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          if (unlikely(!ScantokDouble(cur_modif, &pc.glm_info.max_corr))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --max-corr parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          if (unlikely((pc.glm_info.max_corr < 0.0) || (pc.glm_info.max_corr > 1.0))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --max-corr parameter '%s' (must be in [0, 1]).\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
        } else if (strequal_k_unsafe(flagname_p2, "ach-r2-filter")) {
          if (unlikely(pc.freq_rpt_flags & kfAlleleFreqColMinimac3R2)) {
            logerrputs("Error: --freq minimac3r2 output and --mach-r2-filter cannot be used together.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (param_ct) {
            const char* cur_modif = argvk[arg_idx + 1];
            if (unlikely(!ScantokDouble(cur_modif, &pc.mach_r2_min))) {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --mach-r2-filter min parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
            if (unlikely(pc.mach_r2_min < 0.0)) {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --mach-r2-filter min parameter '%s' (must be nonnegative).\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
            if (param_ct == 2) {
              cur_modif = argvk[arg_idx + 2];
              if (unlikely((!ScantokDouble(cur_modif, &pc.mach_r2_max)) || (pc.mach_r2_max == 0.0))) {
                snprintf(g_logbuf, kLogbufSize, "Error: Invalid --mach-r2-filter max parameter '%s'.\n", cur_modif);
                goto main_ret_INVALID_CMDLINE_WWA;
              }
            } else {
              pc.mach_r2_max = 2.0;
            }
            if (unlikely(pc.mach_r2_max < pc.mach_r2_min)) {
              logerrputs("Error: --mach-r2-filter min parameter cannot be larger than max parameter.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
          } else {
            pc.mach_r2_min = 0.1;
          }
          pc.filter_flags |= kfFilterPvarReq;
          pc.dependency_flags |= kfFilterAllReq | kfFilterNoSplitChr;
        } else if (strequal_k_unsafe(flagname_p2, "inimac3-r2-filter")) {
          if (unlikely(pc.mach_r2_max != 0.0)) {
            logerrputs("Error: --mach-r2-filter and --minimac3-r2-filter cannot be used together.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(pc.freq_rpt_flags & kfAlleleFreqColMachR2)) {
            logerrputs("Error: --freq machr2 output and --minimac3-r2-filter cannot be used together.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          if (unlikely(!ScantokDouble(cur_modif, &pc.minimac3_r2_min))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --minimac3-r2-filter min parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          if (unlikely(pc.minimac3_r2_min < 0.0)) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --minimac3-r2-filter min parameter '%s' (must be nonnegative).\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          if (param_ct == 2) {
            cur_modif = argvk[arg_idx + 2];
            if (unlikely((!ScantokDouble(cur_modif, &pc.minimac3_r2_max)) || (pc.minimac3_r2_max == 0.0))) {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --minimac3-r2-filter max parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          } else {
            pc.minimac3_r2_max = 1.0;
          }
          if (unlikely(pc.minimac3_r2_max < pc.minimac3_r2_min)) {
            logerrputs("Error: --minimac3-r2-filter min parameter cannot be larger than max parameter.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          pc.filter_flags |= kfFilterPvarReq;
          pc.dependency_flags |= kfFilterAllReq | kfFilterNoSplitChr;
        } else if (strequal_k_unsafe(flagname_p2, "issing-code")) {
          if (unlikely(!(xload & (kfXloadOxGen | kfXloadOxBgen)))) {
            // could technically support pure .sample -> .fam/.psam, but let's
            // keep this simple
            logerrputs("Error: --missing-code must be used with --data/--gen/--bgen.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = CmdlineAllocString(param_ct? argvk[arg_idx + 1] : "", argvk[arg_idx], 0x7fffffff, &ox_missing_code);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
        } else if (unlikely(strequal_k_unsafe(flagname_p2, "issing-genotype"))) {
          logerrputs("Error: --missing-genotype flag retired.  Use --input-missing-genotype and/or\n--output-missing-genotype.\n");
          goto main_ret_INVALID_CMDLINE;
        } else if (unlikely(strequal_k_unsafe(flagname_p2, "issing-phenotype"))) {
          logerrputs("Error: --missing-phenotype flag retired.  Use --input-missing-phenotype and/or\n--output-missing-phenotype.\n");
          goto main_ret_INVALID_CMDLINE;
        } else if (strequal_k_unsafe(flagname_p2, "issing-catname")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          const uint32_t cur_modif_slen = strlen(cur_modif);
          double dxx;
          if (unlikely(ScanadvDouble(cur_modif, &dxx) || IsNanStr(cur_modif, cur_modif_slen))) {
            logerrputs("Error: --missing-catname string cannot be 'NA' or start with a number.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(cur_modif_slen > 31)) {
            logerrputs("Error: --missing-catname string too long (max 31 chars).\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          memcpy(g_missing_catname, cur_modif, cur_modif_slen + 1);
        } else if (strequal_k_unsafe(flagname_p2, "ouse")) {
          if (unlikely(chr_info.chrset_source)) {
            logerrputs("Error: Conflicting chromosome-set flags.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          chr_info.chrset_source = kChrsetSourceCmdline;
          chr_info.autosome_ct = 19;
          chr_info.xymt_codes[0] = 20;
          chr_info.xymt_codes[1] = 21;
          chr_info.xymt_codes[2] = UINT32_MAXM1;
          chr_info.xymt_codes[3] = UINT32_MAXM1;
          chr_info.xymt_codes[4] = UINT32_MAXM1;
          chr_info.xymt_codes[5] = UINT32_MAXM1;
          chr_info.haploid_mask[0] = 0x300000;
          goto main_param_zero;
        } else if (unlikely(strequal_k_unsafe(flagname_p2, "ake-grm"))) {
          logerrputs("Error: --make-grm has been retired due to inconsistent meaning across GCTA\nversions.  Use --make-grm-list or --make-grm-bin.\n");
          goto main_ret_INVALID_CMDLINE;
        } else if (strequal_k_unsafe(flagname_p2, "ake-grm-bin")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 3))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          pc.grm_flags |= kfGrmNoIdHeader | kfGrmBin;
          for (uint32_t param_idx = 1; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "cov", cur_modif_slen)) {
              pc.grm_flags |= kfGrmCov;
            } else if (strequal_k(cur_modif, "meanimpute", cur_modif_slen)) {
              pc.grm_flags |= kfGrmMeanimpute;
            } else if (strequal_k(cur_modif, "id-header", cur_modif_slen) ||
                       strequal_k(cur_modif, "idheader", cur_modif_slen)) {
              pc.grm_flags &= ~kfGrmNoIdHeader;
            } else if (likely(strequal_k(cur_modif, "iid-only", cur_modif_slen))) {
              pc.grm_flags |= kfGrmNoIdHeaderIidOnly;
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --make-grm-bin parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          if (unlikely((pc.grm_flags & (kfGrmNoIdHeader | kfGrmNoIdHeaderIidOnly)) == kfGrmNoIdHeaderIidOnly)) {
            logerrputs("Error: --make-grm-bin 'id-header' and 'iid-only' modifiers cannot be used\ntogether.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          pc.command_flags1 |= kfCommand1MakeRel;
          pc.dependency_flags |= kfFilterAllReq;
        } else if (strequal_k_unsafe(flagname_p2, "ake-grm-gz") || strequal_k_unsafe(flagname_p2, "ake-grm-list")) {
          if (unlikely(pc.command_flags1 & kfCommand1MakeRel)) {
            if (pc.grm_flags & kfGrmBin) {
              logerrputs("Error: --make-grm-list cannot be used with --make-grm-bin.\n");
            } else {
              logerrputs("Error: --make-grm-list cannot be used with --make-grm-gz.\n");
            }
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 4))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          uint32_t compress_stream_type = 0;  // 1 = no-gz, 2 = zs
          pc.grm_flags |= kfGrmNoIdHeader;
          for (uint32_t param_idx = 1; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "cov", cur_modif_slen)) {
              pc.grm_flags |= kfGrmCov;
            } else if (strequal_k(cur_modif, "meanimpute", cur_modif_slen)) {
              pc.grm_flags |= kfGrmMeanimpute;
            } else if (strequal_k(cur_modif, "no-gz", cur_modif_slen)) {
              if (unlikely(compress_stream_type)) {
                logerrputs("Error: Multiple --make-grm-list compression type modifiers.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              compress_stream_type = 1;
              pc.grm_flags |= kfGrmListNoGz;
            } else if (strequal_k(cur_modif, "zs", cur_modif_slen)) {
              if (unlikely(compress_stream_type)) {
                logerrputs("Error: Multiple --make-grm-list compression type modifiers.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              compress_stream_type = 2;
              pc.grm_flags |= kfGrmListZs;
            } else if (strequal_k(cur_modif, "id-header", cur_modif_slen) ||
                       strequal_k(cur_modif, "idheader", cur_modif_slen)) {
              pc.grm_flags &= ~kfGrmNoIdHeader;
            } else if (likely(strequal_k(cur_modif, "iid-only", cur_modif_slen))) {
              pc.grm_flags |= kfGrmNoIdHeaderIidOnly;
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --make-grm-list parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          if (flagname_p2[8] == 'g') {
            if (unlikely(!compress_stream_type)) {
              // screw it, life is too much better with multithreaded .zst
              logerrputs("Error: --make-grm-list no longer supports gzipped output.  Use 'zs' for\nzstd-compressed output (much faster), or use PLINK 1.9 for this function.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
            logerrputs("Warning: --make-grm-gz has been renamed to --make-grm-list.\n");
          } else if (!compress_stream_type) {
            compress_stream_type = 1;
            pc.grm_flags |= kfGrmListNoGz;
          }
          if (unlikely((pc.grm_flags & (kfGrmNoIdHeader | kfGrmNoIdHeaderIidOnly)) == kfGrmNoIdHeaderIidOnly)) {
            logerrputs("Error: --make-grm-list 'id-header' and 'iid-only' modifiers cannot be used\ntogether.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          pc.command_flags1 |= kfCommand1MakeRel;
          pc.dependency_flags |= kfFilterAllReq;
        } else if (strequal_k_unsafe(flagname_p2, "ake-rel")) {
          if (unlikely(pc.command_flags1 & kfCommand1MakeRel)) {
            logerrputs("Error: --make-rel cannot be used with --make-grm-list/--make-grm-bin.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 4))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          for (uint32_t param_idx = 1; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "cov", cur_modif_slen)) {
              pc.grm_flags |= kfGrmCov;
            } else if (strequal_k(cur_modif, "meanimpute", cur_modif_slen)) {
              pc.grm_flags |= kfGrmMeanimpute;
            } else if (strequal_k(cur_modif, "zs", cur_modif_slen)) {
              if (unlikely(pc.grm_flags & kfGrmMatrixEncodemask)) {
                logerrputs("Error: Multiple --make-rel encoding modifiers.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              pc.grm_flags |= kfGrmMatrixZs;
            } else if (unlikely(strequal_k(cur_modif, "no-idheader", cur_modif_slen))) {
              logerrputs("Error: --make-rel 'no-idheader' modifier retired.  Use --no-id-header instead.\n");
              goto main_ret_INVALID_CMDLINE_A;
            } else if (strequal_k(cur_modif, "bin", cur_modif_slen)) {
              if (unlikely(pc.grm_flags & kfGrmMatrixEncodemask)) {
                logerrputs("Error: Multiple --make-rel encoding modifiers.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              pc.grm_flags |= kfGrmMatrixBin;
            } else if (strequal_k(cur_modif, "bin4", cur_modif_slen)) {
              if (unlikely(pc.grm_flags & kfGrmMatrixEncodemask)) {
                logerrputs("Error: Multiple --make-rel encoding modifiers.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              pc.grm_flags |= kfGrmMatrixBin4;
            } else if (strequal_k(cur_modif, "square", cur_modif_slen)) {
              if (unlikely(pc.grm_flags & kfGrmMatrixShapemask)) {
                logerrputs("Error: Multiple --make-rel shape modifiers.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              pc.grm_flags |= kfGrmMatrixSq;
            } else if (strequal_k(cur_modif, "square0", cur_modif_slen)) {
              if (unlikely(pc.grm_flags & kfGrmMatrixShapemask)) {
                logerrputs("Error: Multiple --make-rel shape modifiers.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              pc.grm_flags |= kfGrmMatrixSq0;
            } else if (likely(strequal_k(cur_modif, "triangle", cur_modif_slen))) {
              if (unlikely(pc.grm_flags & kfGrmMatrixShapemask)) {
                logerrputs("Error: Multiple --make-rel shape modifiers.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              pc.grm_flags |= kfGrmMatrixTri;
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --make-rel parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          if (!(pc.grm_flags & kfGrmMatrixShapemask)) {
            if (pc.grm_flags & (kfGrmMatrixBin | kfGrmMatrixBin4)) {
              pc.grm_flags |= kfGrmMatrixSq;
            } else {
              pc.grm_flags |= kfGrmMatrixTri;
            }
          }
          pc.command_flags1 |= kfCommand1MakeRel;
          pc.dependency_flags |= kfFilterAllReq;
        } else if (strequal_k_unsafe(flagname_p2, "ap")) {
          if (unlikely(load_params || (xload & (~kfXloadPlink1Dosage)))) {
            goto main_ret_INVALID_CMDLINE_INPUT_CONFLICT;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          const uint32_t slen = strlen(cur_modif);
          if (unlikely(slen > kPglFnamesize - 1)) {
            logerrputs("Error: --map filename too long.\n");
            goto main_ret_OPEN_FAIL;
          }
          memcpy(pvarname, cur_modif, slen + 1);
          xload |= kfXloadMap;
        } else if (strequal_k_unsafe(flagname_p2, "within")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          if (unlikely(ScanPosintCappedx(cur_modif, kMaxLongLine / 2, &pc.mwithin_val))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --mwithin parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
        } else if (strequal_k_unsafe(flagname_p2, "filter")) {
          if (unlikely(!pc.keep_fcol_fname)) {
            logerrputs("Error: --mfilter must be used with --keep-fcol.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(pc.keep_fcol_name)) {
            logerrputs("Error: --mfilter can't be used with --keep-fcol-name.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(pc.keep_fcol_num)) {
            logerrputs("Error: --mfilter can't be used with --keep-fcol-num.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          logerrputs("Warning: --mfilter flag deprecated.  Use --keep-fcol-num or --keep-fcol-name\ninstead.  (Note that --keep-fcol-num does not add 2 to the column number.)\n");
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          uint32_t mfilter_arg;
          if (unlikely(ScanPosintDefcapx(cur_modif, &mfilter_arg))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --mfilter parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          pc.keep_fcol_num = mfilter_arg + 2;
        } else if (strequal_k_unsafe(flagname_p2, "ax-alleles")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          if (unlikely(ScanPosintDefcapx(cur_modif, &pc.filter_max_allele_ct))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --max-alleles parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          pc.filter_flags |= kfFilterPvarReq;
        } else if (strequal_k_unsafe(flagname_p2, "in-alleles")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          if (unlikely(ScanPosintDefcapx(cur_modif, &pc.filter_min_allele_ct))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --min-alleles parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          if (unlikely(pc.filter_min_allele_ct > pc.filter_max_allele_ct)) {
            logerrputs("Error: --min-alleles argument can't be larger than --max-alleles argument.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          if (pc.filter_min_allele_ct == 1) {
            pc.filter_min_allele_ct = 0;
          }
          pc.filter_flags |= kfFilterPvarReq;
        } else if (likely(strequal_k_unsafe(flagname_p2, "pheno"))) {
          logerrputs("Warning: --mpheno flag deprecated.  Use --pheno-col-nums instead.  (Note that\n--pheno-col-nums does not add 2 to the column number(s).)\n");
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          uint32_t mpheno_arg;
          if (unlikely(ScanPosintDefcapx(cur_modif, &mpheno_arg))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --mpheno parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          // add two to the number, convert it back to a string, and act as if
          // --pheno-col-nums was used.  See ParseNameRanges().
          const uint32_t pheno_col_nums_arg = mpheno_arg + 2;
          const uint32_t name_max_blen = UintSlen(pheno_col_nums_arg) + 1;
          if (unlikely(pgl_malloc(name_max_blen + 1, &pc.pheno_range_list.names))) {
            goto main_ret_NOMEM;
          }
          pc.pheno_range_list.name_max_blen = name_max_blen;
          pc.pheno_range_list.name_ct = 1;
          char* write_iter = u32toa(pheno_col_nums_arg, pc.pheno_range_list.names);
          *write_iter++ = '\0';
          pc.pheno_range_list.starts_range = R_CAST(unsigned char*, write_iter);
          *write_iter = '\0';
          pc.misc_flags |= kfMiscPhenoColNums;
        } else {
          goto main_ret_INVALID_CMDLINE_UNRECOGNIZED;
        }
        break;

      case 'n':
        if (strequal_k_unsafe(flagname_p2, "o-fid")) {
          pc.fam_cols &= ~kfFamCol1;
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "o-parents")) {
          pc.fam_cols &= ~kfFamCol34;
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "o-sex")) {
          pc.fam_cols &= ~kfFamCol5;
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "o-psam-pheno")) {
          // move this out of fam_cols?
          pc.fam_cols &= ~kfFamCol6;
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "onfounders")) {
          pc.misc_flags |= kfMiscNonfounders;
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "ot-chr")) {
          if (unlikely(pc.varid_from)) {
            logerrputs("Error: --from/--to cannot be used with --autosome{-par} or --{not-}chr.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(pc.from_bp != -1)) {
            logerrputs("Error: --from-bp/-kb/-mb and --to-bp/-kb/-mb cannot be used with --not-chr.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }

          // allowed:
          //   --allow-extra-chr --chr 5-22 bobs_chrom --not-chr 17
          // allowed:
          //   --allow-extra-chr --not-chr 12-17 bobs_chrom
          // does not make sense, disallowed:
          //   --allow-extra-chr --chr 5-22 --not-chr bobs_chrom
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 0x7fffffff))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }

          // --allow-extra-chr present, --chr/--autosome{-xy} not present
          const uint32_t aec_and_no_chr_include = ((pc.misc_flags / kfMiscAllowExtraChrs) & 1) && (!chr_info.is_include_stack);
          reterr = ParseChrRanges(&(argvk[arg_idx]), flagname_p, errstr_append, param_ct, aec_and_no_chr_include, kChrRawEnd - (kChrExcludeWords * kBitsPerWord), '-', &chr_info, chr_info.chr_exclude);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          notchr_present = 1;
          // remaining processing now postponed to FinalizeChrset()
        } else if (strequal_k_unsafe(flagname_p2, "ew-id-max-allele-len")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          if (unlikely(ScanPosintCappedx(cur_modif, kMaxIdSlen - 2, &pc.new_variant_id_max_allele_slen))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --new-id-max-allele-len length parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          if (param_ct == 2) {
            cur_modif = argvk[arg_idx + 2];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "missing", cur_modif_slen)) {
              pc.misc_flags |= kfMiscNewVarIdOverflowMissing;
            } else if (strequal_k(cur_modif, "truncate", cur_modif_slen)) {
              pc.misc_flags |= kfMiscNewVarIdOverflowTruncate;
            } else if (unlikely(!strequal_k(cur_modif, "error", cur_modif_slen))) {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --new-id-max-allele-len parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
        } else if (strequal_k_unsafe(flagname_p2, "ormalize")) {
          // Prevent a confusing order-of-operations dependency.
          if (unlikely(pc.alt1_allele_flag)) {
            logerrputs("Error: --normalize cannot be used with --ref-allele/--alt1-allele.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(!pc.fa_fname)) {
            logerrputs("Error: --normalize requires --fa.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (param_ct) {
            const char* cur_modif = argvk[arg_idx + 1];
            if (unlikely(strcmp(cur_modif, "list"))) {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --normalize parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
            pc.fa_flags |= kfFaNormalizeList;
          }
          pc.fa_flags |= kfFaNormalize;
          pc.dependency_flags |= kfFilterPvarReq;
        } else if (strequal_k_unsafe(flagname_p2, "o-input-missing-phenotype")) {
          if (pc.missing_pheno != -9) {
            logerrputs("Error: --input-missing-phenotype and --no-input-missing-phenotype don't make\nsense together.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          pc.missing_pheno = 0;
          goto main_param_zero;
        } else if (likely(strequal_k_unsafe(flagname_p2, "o-id-header"))) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (param_ct) {
            const char* cur_modif = argvk[arg_idx + 1];
            if (unlikely(strcmp(cur_modif, "iid-only"))) {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --no-id-header parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
            pc.misc_flags |= kfMiscNoIdHeaderIidOnly;
          }
          pc.misc_flags |= kfMiscNoIdHeader;
        } else {
          goto main_ret_INVALID_CMDLINE_UNRECOGNIZED;
        }
        break;

      case 'o':
        if (strequal_k_unsafe(flagname_p2, "utput-chr")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* mt_code = argvk[arg_idx + 1];
          const uint32_t code_slen = strlen(mt_code);
          if (strequal_k(mt_code, "M", code_slen)) {
            chr_info.output_encoding = kfChrOutputM;
          } else if (strequal_k(mt_code, "MT", code_slen)) {
            chr_info.output_encoding = kfChrOutputMT;
          } else if (strequal_k(mt_code, "0M", code_slen)) {
            chr_info.output_encoding = kfChrOutput0M;
          } else if (strequal_k(mt_code, "chr26", code_slen)) {
            chr_info.output_encoding = kfChrOutputPrefix;
          } else if (strequal_k(mt_code, "chrM", code_slen)) {
            chr_info.output_encoding = kfChrOutputPrefix | kfChrOutputM;
          } else if (strequal_k(mt_code, "chrMT", code_slen)) {
            chr_info.output_encoding = kfChrOutputPrefix | kfChrOutputMT;
          } else if (likely(strequal_k(mt_code, "26", code_slen))) {
            chr_info.output_encoding = kfChrOutput0;
          } else {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --output-chr parameter '%s'.\n", mt_code);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
        } else if (strequal_k_unsafe(flagname_p2, "utput-min-p")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          if (unlikely((!ScantokLn(cur_modif, &pc.output_min_ln)) || (pc.output_min_ln >= 0.0))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --output-min-p parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
        } else if (strequal_k_unsafe(flagname_p2, "xford-single-chr")) {
          if (unlikely(!(xload & (kfXloadOxGen | kfXloadOxBgen)))) {
            logerrputs("Error: --oxford-single-chr must be used with .gen/.bgen input.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(oxford_import_flags & kfOxfordImportBgenSnpIdChr)) {
            logerrputs("Error: --oxford-single-chr cannot be used with --bgen 'snpid-chr'.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          if (!(pc.misc_flags & kfMiscAllowExtraChrs)) {
            if (unlikely(IsI32Neg(GetChrCodeRaw(cur_modif)))) {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --oxford-single-chr chromosome code '%s'. (Did you forget --allow-extra-chr?)\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          reterr = CmdlineAllocString(cur_modif, argvk[arg_idx], kMaxIdSlen, &import_single_chr_str);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
        } else if (strequal_k_unsafe(flagname_p2, "utput-missing-genotype")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          output_missing_geno_char = ExtractCharParam(cur_modif);
          if (unlikely(ctou32(output_missing_geno_char) <= ' ')) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --output-missing-genotype parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
        } else if (strequal_k_unsafe(flagname_p2, "utput-missing-phenotype")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          const uint32_t cur_modif_slen = strlen(cur_modif);
          if (unlikely(cur_modif_slen > 31)) {
            logerrputs("Error: --output-missing-phenotype string too long (max 31 chars).\n");
            goto main_ret_INVALID_CMDLINE;
          }
          memcpy(g_output_missing_pheno, cur_modif, cur_modif_slen + 1);
        } else if (unlikely(!strequal_k_unsafe(flagname_p2, "ut"))) {
          // --out is a special case due to logging
          goto main_ret_INVALID_CMDLINE_UNRECOGNIZED;
        }
        break;

      case 'p':
        if (strequal_k_unsafe(flagname_p2, "file")) {
          if (unlikely(load_params || xload)) {
            // currently only possible with --bcf, --bfile, --pfile
            goto main_ret_INVALID_CMDLINE_INPUT_CONFLICT;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          uint32_t fname_modif_idx = 1;
          if (param_ct == 2) {
            if (unlikely(CheckExtraParam(&(argvk[arg_idx]), "vzs", &fname_modif_idx))) {
              goto main_ret_INVALID_CMDLINE_A;
            }
          }
          const char* fname_prefix = argvk[arg_idx + fname_modif_idx];
          const uint32_t slen = strlen(fname_prefix);
          if (unlikely(slen > (kPglFnamesize - 11))) {
            logerrputs("Error: --pfile parameter too long.\n");
            goto main_ret_OPEN_FAIL;
          }
          snprintf(memcpya(pgenname, fname_prefix, slen), 10, ".pgen");
          snprintf(memcpya(psamname, fname_prefix, slen), 10, ".psam");
          char* pvarname_end = memcpya(pvarname, fname_prefix, slen);
          pvarname_end = Stpcpy(pvarname_end, ".pvar");
          if (param_ct == 2) {
            snprintf(pvarname_end, 5, ".zst");
          }
          load_params |= kfLoadParamsPfileAll;
        } else if (strequal_k_unsafe(flagname_p2, "gen")) {
          if (unlikely(xload)) {
            goto main_ret_INVALID_CMDLINE_INPUT_CONFLICT;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          load_params |= kfLoadParamsPgen;
          const char* fname = argvk[arg_idx + 1];
          const uint32_t slen = strlen(fname);
          if (unlikely(slen > (kPglFnamesize - 2))) {
            logerrputs("Error: --pgen parameter too long.\n");
            goto main_ret_OPEN_FAIL;
          }
          memcpy(pgenname, fname, slen + 1);
        } else if (strequal_k_unsafe(flagname_p2, "sam")) {
          if (unlikely(xload & (~(kfXloadVcf | kfXloadBcf | kfXloadPlink1Dosage | kfXloadMap)))) {
            goto main_ret_INVALID_CMDLINE_INPUT_CONFLICT;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          load_params |= kfLoadParamsPsam;
          const char* fname = argvk[arg_idx + 1];
          const uint32_t slen = strlen(fname);
          if (unlikely(slen > (kPglFnamesize - 2))) {
            logerrputs("Error: --psam parameter too long.\n");
            goto main_ret_OPEN_FAIL;
          }
          memcpy(psamname, fname, slen + 1);
        } else if (strequal_k_unsafe(flagname_p2, "var")) {
          if (unlikely(xload)) {
            goto main_ret_INVALID_CMDLINE_INPUT_CONFLICT;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          load_params |= kfLoadParamsPvar;
          const char* fname = argvk[arg_idx + 1];
          const uint32_t slen = strlen(fname);
          if (unlikely(slen > (kPglFnamesize - 2))) {
            logerrputs("Error: --pvar parameter too long.\n");
            goto main_ret_OPEN_FAIL;
          }
          memcpy(pvarname, fname, slen + 1);
        } else if (strequal_k_unsafe(flagname_p2, "heno")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          uint32_t fname_idx = 1;
          if (param_ct == 2) {
            if (unlikely(CheckExtraParam(&(argvk[arg_idx]), "iid-only", &fname_idx))) {
              goto main_ret_INVALID_CMDLINE_A;
            }
            pc.misc_flags |= kfMiscPhenoIidOnly;
          }
          reterr = AllocFname(argvk[arg_idx + fname_idx], flagname_p, 0, &pc.pheno_fname);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.dependency_flags |= kfFilterPsamReq;
        } else if (strequal_k_unsafe(flagname_p2, "heno-col-nums")) {
          if (unlikely(!pc.pheno_fname)) {
            logerrputs("Error: --pheno-col-nums must be used with --pheno.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(pc.pheno_range_list.name_ct)) {
            logerrputs("Error: --pheno-col-nums can't be used with --mpheno.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          reterr = ParseNameRanges(&(argvk[arg_idx]), errstr_append, param_ct, 1, '-', &pc.pheno_range_list);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.misc_flags |= kfMiscPhenoColNums;
        } else if (strequal_k_unsafe(flagname_p2, "heno-name")) {
          if (unlikely(pc.pheno_range_list.name_ct)) {
            logerrputs("Error: --pheno-name can't be used with --pheno-col-nums.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          // can now be used without --pheno
          reterr = ParseNameRanges(&(argvk[arg_idx]), errstr_append, param_ct, 0, range_delim, &pc.pheno_range_list);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
        } else if (strequal_k_unsafe(flagname_p2, "arallel")) {
          if (unlikely(pc.king_flags & kfKingMatrixSq)) {
            logerrputs("Error: --parallel cannot be used with \"--make-king square\".  Use \"--make-king\nsquare0\" or plain --make-king instead.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely((pc.king_cutoff != -1) && (!king_cutoff_fprefix))) {
            logerrputs("Error: --parallel cannot be used with --king-cutoff.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(pc.grm_flags & kfGrmMatrixSq)) {
            logerrputs("Error: --parallel cannot be used with \"--make-rel square\".  Use \"--make-rel\nsquare0\" or plain --make-rel instead.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 2, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (unlikely(ScanPosintCappedx(argvk[arg_idx + 1], kParallelMax, &pc.parallel_idx))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --parallel job index '%s'.\n", argvk[arg_idx + 1]);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          if (unlikely(ScanPosintCappedx(argvk[arg_idx + 2], kParallelMax, &pc.parallel_tot) || (pc.parallel_tot == 1) || (pc.parallel_tot < pc.parallel_idx))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --parallel total job count '%s'.\n", argvk[arg_idx + 2]);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          --pc.parallel_idx;  // internal 0..(n-1) indexing
        } else if (strequal_k_unsafe(flagname_p2, "arameters")) {
          if (unlikely(!(pc.command_flags1 & kfCommand1Glm))) {
            logerrputs("Error: --parameters must be used with --glm.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          reterr = ParseNameRanges(&(argvk[arg_idx]), errstr_append, param_ct, 1, '-', &pc.glm_info.parameters_range_list);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
        } else if (strequal_k_unsafe(flagname_p2, "filter")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          if (unlikely(!ScantokLn(cur_modif, &pc.ln_pfilter))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --pfilter parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          if (unlikely((pc.ln_pfilter == -DBL_MAX) || (pc.ln_pfilter > 0.0))) {
            logerrputs("Error: --pfilter threshold must be in (0, 1].\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
        } else if (strequal_k_unsafe(flagname_p2, "ca")) {
#ifdef NOLAPACK
          logerrputs("Error: --pca requires " PROG_NAME_STR " to be built with LAPACK.\n");
          goto main_ret_INVALID_CMDLINE;
#endif
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 6))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          uint32_t explicit_scols = 0;
          uint32_t is_var_wts = 0;
          for (uint32_t param_idx = 1; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "approx", cur_modif_slen)) {
              pc.pca_flags |= kfPcaApprox;
            } else if (strequal_k(cur_modif, "meanimpute", cur_modif_slen)) {
              pc.pca_flags |= kfPcaMeanimpute;
            } else if (strequal_k(cur_modif, "var-wts", cur_modif_slen)) {
              logerrputs("Warning: The 'var-wts' modifier is deprecated, and will stop working in alpha\n3.  Switch to the forward-compatible 'biallelic-var-wts' modifier if you are\nworking with only biallelic variants and want one-line-per-variant output.\n");
              is_var_wts = 1;
              pc.pca_flags |= kfPcaIgnoreBiallelicVarWtsRestriction;
            } else if (strequal_k(cur_modif, "biallelic-var-wts", cur_modif_slen)) {
              is_var_wts = 1;
            } else if (strequal_k(cur_modif, "vzs", cur_modif_slen)) {
              pc.pca_flags |= kfPcaVarZs;
            } else if (StrStartsWith(cur_modif, "scols=", cur_modif_slen)) {
              if (unlikely(explicit_scols)) {
                logerrputs("Error: Multiple --pca scols= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              reterr = ParseColDescriptor(&(cur_modif[strlen("scols=")]), "maybefid\0fid\0maybesid\0sid\0", "pca scols", kfPcaScolMaybefid, kfPcaScolDefault, 0, &pc.pca_flags);
              if (unlikely(reterr)) {
                goto main_ret_1;
              }
              explicit_scols = 1;
            } else if (StrStartsWith(cur_modif, "vcols=", cur_modif_slen)) {
              if (unlikely(pc.pca_flags & kfPcaVcolAll)) {
                logerrputs("Error: Multiple --pca vcols= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              reterr = ParseColDescriptor(&(cur_modif[strlen("vcols=")]), "chrom\0pos\0ref\0alt1\0alt\0maj\0nonmaj\0", "pca vcols", kfPcaVcolChrom, kfPcaVcolDefault, 1, &pc.pca_flags);
              if (unlikely(reterr)) {
                goto main_ret_1;
              }
            } else if (unlikely(strequal_k(cur_modif, "sid", cur_modif_slen))) {
              logerrputs("Error: --pca 'sid' modifier retired.  Use --pca scols= instead.\n");
              goto main_ret_INVALID_CMDLINE_A;
            } else if (unlikely(strequal_k(cur_modif, "allele-wts", cur_modif_slen))) {
              logerrputs("Error: --pca 'allele-wts' modifier requires alpha 3 or later.\n");
              goto main_ret_INVALID_CMDLINE_A;
            } else {
              if (unlikely(pc.pca_ct || ScanPosintDefcapx(cur_modif, &pc.pca_ct))) {
                logerrputs("Error: Invalid --pca parameter sequence.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              if (unlikely(pc.pca_ct > 8000)) {
                // this slightly simplifies output buffering.
                // lower limit for randomized algorithm?
                // (just let memory allocation fail for now...)
                logerrputs("Error: --pca does not support more than 8000 PCs.\n");
                goto main_ret_INVALID_CMDLINE;
              }
            }
          }
          if (pc.pca_flags & kfPcaApprox) {
            if (unlikely(pc.pca_ct > 100)) {
              // double-precision overflow too likely
              logerrputs("Error: --pca approx does not support more than 100 PCs.\n");
              goto main_ret_INVALID_CMDLINE;
            }
          } else {
            // todo: if --make-rel/--make-grm present, verify consistency
            if (unlikely(pc.parallel_tot != 1)) {
              logerrputs("Error: Non-approximate --pca cannot be used with --parallel.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
            const uint32_t pca_meanimpute = (pc.pca_flags / kfPcaMeanimpute) & 1;
            if (pc.command_flags1 & kfCommand1MakeRel) {
              if (unlikely(((pc.grm_flags / kfGrmMeanimpute) & 1) != pca_meanimpute)) {
                logerrputs("Error: --make-rel/--make-grm-list/--make-grm-bin meanimpute setting must match\n--pca meanimpute setting.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              if (unlikely(pc.grm_flags & kfGrmCov)) {
                logerrputs("Error: --make-rel/--make-grm-list/--make-grm-bin cannot be used to compute a\ncovariance matrix in the same run as non-approximate --pca.\n");
                goto main_ret_INVALID_CMDLINE;
              }
            } else {
              if (pca_meanimpute) {
                pc.grm_flags |= kfGrmMeanimpute;
              }
            }
          }
          if (!pc.pca_ct) {
            pc.pca_ct = 10;
          }
          if (!explicit_scols) {
            pc.pca_flags |= kfPcaScolDefault;
          }
          if (!(pc.pca_flags & kfPcaVcolAll)) {
            if (is_var_wts) {
              pc.pca_flags |= kfPcaVcolDefault;
            }
          } else if (unlikely(!is_var_wts)) {
            logerrputs("Error: --pca 'vcols=' has no effect when variant/allele weights have not been\nrequested.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          if (is_var_wts) {
            pc.pca_flags |= kfPcaBiallelicVarWts;
          } else if (unlikely(pc.pca_flags & kfPcaVarZs)) {
            logerrputs("Error: --pca 'vzs' modifier has no effect when variant/allele weights have not\nbeen requested.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          pc.command_flags1 |= kfCommand1Pca;
          pc.dependency_flags |= kfFilterAllReq;
        } else if (strequal_k_unsafe(flagname_p2, "gen-info")) {
          pc.command_flags1 |= kfCommand1PgenInfo;
          pc.dependency_flags |= kfFilterAllReq;
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "hased-r2-filter")) {
          logerrputs("Error: --phased-r2-filter is under development.\n");
          goto main_ret_INVALID_CMDLINE;
        } else if (likely(strequal_k_unsafe(flagname_p2, "heno-quantile-normalize"))) {
          if (param_ct) {
            reterr = AllocAndFlatten(&(argvk[arg_idx + 1]), param_ct, 0x7fffffff, &pc.quantnorm_flattened);
            if (unlikely(reterr)) {
              goto main_ret_1;
            }
          }
          pc.pheno_transform_flags |= kfPhenoTransformQuantnormPheno;
          pc.dependency_flags |= kfFilterPsamReq;
        } else {
          goto main_ret_INVALID_CMDLINE_UNRECOGNIZED;
        }
        break;

      case 'q':
        if (strequal_k_unsafe(flagname_p2, "uantile-normalize")) {
          if (unlikely(pc.pheno_transform_flags & (kfPhenoTransformQuantnormPheno | kfPhenoTransformQuantnormCovar))) {
            logerrputs("Error: --quantile-normalize cannot be used with --pheno-quantile-normalize or\n--covar-quantile-normalize.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          if (param_ct) {
            reterr = AllocAndFlatten(&(argvk[arg_idx + 1]), param_ct, 0x7fffffff, &pc.quantnorm_flattened);
            if (unlikely(reterr)) {
              goto main_ret_1;
            }
          }
          pc.pheno_transform_flags |= kfPhenoTransformQuantnormAll;
          pc.dependency_flags |= kfFilterPsamReq;
        } else if (likely(strequal_k_unsafe(flagname_p2, "-score-range"))) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 2, 6))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocFname(argvk[arg_idx + 1], flagname_p, 0, &pc.score_info.qsr_range_fname);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          reterr = AllocFname(argvk[arg_idx + 2], flagname_p, 0, &pc.score_info.qsr_data_fname);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          uint32_t numeric_param_ct = 0;
          uint32_t qsr_cols[3];
          for (uint32_t param_idx = 3; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "header", cur_modif_slen)) {
              pc.score_info.flags |= kfScoreQsrHeader;
            } else if (strequal_k(cur_modif, "min", cur_modif_slen)) {
              pc.score_info.flags |= kfScoreQsrMin;
            } else {
              if (unlikely(ScanPosintCappedx(cur_modif, kMaxLongLine / 2, &(qsr_cols[numeric_param_ct])))) {
                snprintf(g_logbuf, kLogbufSize, "Error: Invalid --q-score-range parameter '%s'.\n", cur_modif);
                goto main_ret_INVALID_CMDLINE_WWA;
              }
              if (unlikely(numeric_param_ct == 2)) {
                logerrputs("Error: --q-score-range takes at most two numeric parameters.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              if (unlikely(numeric_param_ct && (qsr_cols[0] == qsr_cols[1]))) {
                logerrputs("Error: Identical --q-score-range column indexes.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              ++numeric_param_ct;
            }
          }
          if (numeric_param_ct) {
            pc.score_info.qsr_varid_col_p1 = qsr_cols[0];
          }
          if (numeric_param_ct == 2) {
            pc.score_info.qsr_val_col_p1 = qsr_cols[1];
          } else {
            pc.score_info.qsr_val_col_p1 = pc.score_info.qsr_varid_col_p1 + 1;
          }
          // no dependencies since we enforce --score requirement later
        } else {
          goto main_ret_INVALID_CMDLINE_UNRECOGNIZED;
        }
        break;

      case 'r':
        if (strequal_k_unsafe(flagname_p2, "eal-ref-alleles")) {
          if (unlikely(pc.misc_flags & kfMiscMajRef)) {
            logerrputs("Error: --real-ref-alleles cannot be used with --maj-ref.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          pc.misc_flags |= kfMiscRealRefAlleles;
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "emove")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 0x7fffffff))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocAndFlatten(&(argvk[arg_idx + 1]), param_ct, kPglFnamesize, &pc.remove_fnames);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.filter_flags |= kfFilterPsamReq;
        } else if (strequal_k_unsafe(flagname_p2, "emove-fam")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 0x7fffffff))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocAndFlatten(&(argvk[arg_idx + 1]), param_ct, kPglFnamesize, &pc.removefam_fnames);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.filter_flags |= kfFilterPsamReq;
        } else if (strequal_k_unsafe(flagname_p2, "emove-females")) {
          pc.filter_flags |= kfFilterPsamReq | kfFilterExclFemales;
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "emove-males")) {
          pc.filter_flags |= kfFilterPsamReq | kfFilterExclMales;
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "emove-nosex")) {
          pc.filter_flags |= kfFilterPsamReq | kfFilterExclNosex;
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "ead-freq")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocFname(argvk[arg_idx + 1], flagname_p, 0, &pc.read_freq_fname);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.dependency_flags |= kfFilterAllReq;
        } else if (strequal_k_unsafe(flagname_p2, "equire-pheno")) {
          if (param_ct) {
            reterr = AllocAndFlatten(&(argvk[arg_idx + 1]), param_ct, 0x7fffffff, &pc.require_pheno_flattened);
            if (unlikely(reterr)) {
              goto main_ret_1;
            }
          }
          pc.misc_flags |= kfMiscRequirePheno;
          pc.filter_flags |= kfFilterPsamReq;
        } else if (strequal_k_unsafe(flagname_p2, "equire-covar")) {
          if (param_ct) {
            reterr = AllocAndFlatten(&(argvk[arg_idx + 1]), param_ct, 0x7fffffff, &pc.require_covar_flattened);
            if (unlikely(reterr)) {
              goto main_ret_1;
            }
          }
          pc.misc_flags |= kfMiscRequireCovar;
          pc.filter_flags |= kfFilterPsamReq;
        } else if (strequal_k_unsafe(flagname_p2, "equire-info")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 0x7fffffff))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocAndFlattenCommaDelim(&(argvk[arg_idx + 1]), param_ct, &pc.require_info_flattened);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.filter_flags |= kfFilterPvarReq;
        } else if (strequal_k_unsafe(flagname_p2, "equire-no-info")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 0x7fffffff))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocAndFlattenCommaDelim(&(argvk[arg_idx + 1]), param_ct, &pc.require_no_info_flattened);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.filter_flags |= kfFilterPvarReq;
        } else if (strequal_k_unsafe(flagname_p2, "emove-if")) {
          reterr = ValidateAndAllocCmpExpr(&(argvk[arg_idx + 1]), argvk[arg_idx], param_ct, &pc.remove_if_expr);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.filter_flags |= kfFilterPsamReq;
        } else if (strequal_k_unsafe(flagname_p2, "emove-cats")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocFname(argvk[arg_idx + 1], flagname_p, 0, &pc.remove_cats_fname);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.filter_flags |= kfFilterPsamReq;
        } else if (strequal_k_unsafe(flagname_p2, "emove-cat-names")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 0x7fffffff))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocAndFlatten(&(argvk[arg_idx + 1]), param_ct, kMaxIdBlen, &pc.remove_cat_names_flattened);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.filter_flags |= kfFilterPsamReq;
        } else if (strequal_k_unsafe(flagname_p2, "emove-cat-pheno")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = CmdlineAllocString(argvk[arg_idx + 1], argvk[arg_idx], kMaxIdSlen, &pc.remove_cat_phenoname);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
        } else if (strequal_k_unsafe(flagname_p2, "ef-allele")) {
          if (unlikely(pc.misc_flags & kfMiscMajRef)) {
            logerrputs("Error: --maj-ref cannot be used with --ref-allele/--alt1-allele.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(pc.fa_flags & kfFaNormalize)) {
            logerrputs("Error: --normalize cannot be used with --ref-allele/--alt1-allele.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 5))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* const* sources = &(argvk[arg_idx + 1]);
          if (!strcmp(sources[0], "force")) {
            --param_ct;
            if (unlikely(!param_ct)) {
              logerrputs("Error: Invalid --ref-allele parameter sequence.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
            pc.misc_flags |= kfMiscRefAlleleForce;
            ++sources;
          }
          reterr = Alloc2col(sources, flagname_p, param_ct, &pc.ref_allele_flag);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.dependency_flags |= kfFilterPvarReq;
        } else if (strequal_k_unsafe(flagname_p2, "ef-from-fa")) {
          if (unlikely((pc.misc_flags & kfMiscMajRef) || pc.alt1_allele_flag)) {
            // could allow --alt1-allele later, but keep this simpler for now
            logerrputs("Error: --ref-from-fa cannot be used with --maj-ref or\n--ref-allele/--alt1-allele.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          uint32_t fname_modif_idx = 0;
          for (uint32_t param_idx = 1; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "force", cur_modif_slen)) {
              pc.fa_flags |= kfFaRefFromForce;
            } else if (likely((!fname_modif_idx) && (!pc.fa_fname))) {
              fname_modif_idx = param_idx;
            } else {
              logerrputs("Error: Invalid --ref-from-fa parameter sequence.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
          }
          if (fname_modif_idx) {
            reterr = AllocFname(argvk[arg_idx + fname_modif_idx], flagname_p, 0, &pc.fa_fname);
            if (unlikely(reterr)) {
              goto main_ret_1;
            }
            logerrputs("Warning: Filename-parameter form of --ref-from-fa is deprecated.  Use --fa to\nspecify the .fa file instead.\n");
          } else if (unlikely(!pc.fa_fname)) {
            logerrputs("Error: --ref-from-fa requires --fa.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          pc.fa_flags |= kfFaRefFrom;
          pc.dependency_flags |= kfFilterPvarReq | kfFilterNonrefFlagsNeededSet;
        } else if (strequal_k_unsafe(flagname_p2, "m-dup")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          RmDupMode rmdup_mode = kRmDup0;
          for (uint32_t param_idx = 1; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "list", cur_modif_slen)) {
              pc.command_flags1 |= kfCommand1RmDupList;
            } else if (rmdup_mode != kRmDup0) {
              logerrputs("Error: Invalid --rm-dup parameter sequence.\n");
              goto main_ret_INVALID_CMDLINE_A;
            } else if (strequal_k(cur_modif, "error", cur_modif_slen)) {
              rmdup_mode = kRmDupError;
            } else if (strequal_k(cur_modif, "retain-mismatch", cur_modif_slen)) {
              rmdup_mode = kRmDupRetainMismatch;
            } else if (strequal_k(cur_modif, "exclude-mismatch", cur_modif_slen)) {
              rmdup_mode = kRmDupExcludeMismatch;
            } else if (strequal_k(cur_modif, "exclude-all", cur_modif_slen)) {
              rmdup_mode = kRmDupExcludeAll;
            } else if (likely(strequal_k(cur_modif, "force-first", cur_modif_slen))) {
              rmdup_mode = kRmDupForceFirst;
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --rm-dup parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          if (rmdup_mode < kRmDupExcludeAll) {
            if (rmdup_mode == kRmDup0) {
              rmdup_mode = kRmDupError;
            }
            pc.dependency_flags |= kfFilterOpportunisticPgen;
          }
          pc.rmdup_mode = rmdup_mode;
          pc.dependency_flags |= kfFilterPvarReq;
        } else if (strequal_k_unsafe(flagname_p2, "ecover-var-ids")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 4))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          uint32_t fname_idx = 0;
          for (uint32_t param_idx = 1; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "strict-bim-order", cur_modif_slen)) {
              pc.recover_var_ids_flags |= kfRecoverVarIdsStrictBimOrder;
            } else if (strequal_k(cur_modif, "rigid", cur_modif_slen)) {
              pc.recover_var_ids_flags |= kfRecoverVarIdsRigid;
            } else if (strequal_k(cur_modif, "force", cur_modif_slen)) {
              pc.recover_var_ids_flags |= kfRecoverVarIdsForce;
            } else if (strequal_k(cur_modif, "partial", cur_modif_slen)) {
              pc.recover_var_ids_flags |= kfRecoverVarIdsPartial;
            } else {
              if (unlikely(fname_idx)) {
                logerrputs("Error: Invalid --recover-var-ids parameter sequence.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              fname_idx = param_idx;
            }
          }
          if (unlikely(!fname_idx)) {
            logerrputs("Error: No filename provided for --recover-var-ids.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely((pc.recover_var_ids_flags & (kfRecoverVarIdsRigid | kfRecoverVarIdsForce)) == (kfRecoverVarIdsRigid | kfRecoverVarIdsForce))) {
            logerrputs("Error: --recover-var-ids 'rigid' and 'force' modifiers cannot be used\ntogether.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          reterr = AllocFname(argvk[arg_idx + fname_idx], flagname_p, 0, &pc.recover_var_ids_fname);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.dependency_flags |= kfFilterPvarReq;
        } else if (strequal_k_unsafe(flagname_p2, "andmem")) {
          randmem = 1;
          goto main_param_zero;
        } else if (likely(strequal_k_unsafe(flagname_p2, "ice"))) {
          if (unlikely(chr_info.chrset_source)) {
            logerrputs("Error: Conflicting chromosome-set flags.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          chr_info.chrset_source = kChrsetSourceCmdline;
          chr_info.autosome_ct = 12;
          chr_info.xymt_codes[0] = UINT32_MAXM1;
          chr_info.xymt_codes[1] = UINT32_MAXM1;
          chr_info.xymt_codes[2] = UINT32_MAXM1;
          chr_info.xymt_codes[3] = UINT32_MAXM1;
          chr_info.xymt_codes[4] = UINT32_MAXM1;
          chr_info.xymt_codes[5] = UINT32_MAXM1;
          chr_info.haploid_mask[0] = 0x1fff;
          goto main_param_zero;
        } else {
          goto main_ret_INVALID_CMDLINE_UNRECOGNIZED;
        }
        break;

      case 's':
        if (strequal_k_unsafe(flagname_p2, "eed")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 0x7fffffff))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          rseed_ct = param_ct;
          if (unlikely(pgl_malloc(param_ct * sizeof(int32_t), &rseeds))) {
            goto main_ret_NOMEM;
          }
          for (uint32_t param_idx = 1; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            if (unlikely(ScanUintCappedx(cur_modif, UINT32_MAX, &(rseeds[param_idx - 1])))) {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --seed parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
        } else if (strequal_k_unsafe(flagname_p2, "plit-par")) {
          if (unlikely(pc.misc_flags & (kfMiscMergePar | kfMiscMergeX))) {
            logerrputs("Error: --split-par cannot be used with --merge-par/--merge-x.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (param_ct == 1) {
            const char* build_code = argvk[arg_idx + 1];
            const uint32_t code_slen = strlen(build_code);
            if (strequal_k(build_code, "b38", code_slen) ||
                strequal_k(build_code, "hg38", code_slen)) {
              pc.splitpar_bound1 = 2781479;
              pc.splitpar_bound2 = 155701383;
            } else if (strequal_k(build_code, "b37", code_slen) ||
                       strequal_k(build_code, "hg19", code_slen)) {
              pc.splitpar_bound1 = 2699520;
              pc.splitpar_bound2 = 154931044;
            } else if (likely(strequal_k(build_code, "b36", code_slen) ||
                              strequal_k(build_code, "hg18", code_slen))) {
              pc.splitpar_bound1 = 2709521;
              pc.splitpar_bound2 = 154584237;
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Unrecognized --split-par build code '%s'.\n", build_code);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          } else {
            if (unlikely(ScanUintDefcapx(argvk[arg_idx + 1], &pc.splitpar_bound1))) {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --split-par parameter '%s'.\n", argvk[arg_idx + 1]);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
            if (unlikely(ScanUintDefcapx(argvk[arg_idx + 2], &pc.splitpar_bound2) || (pc.splitpar_bound2 <= pc.splitpar_bound1))) {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --split-par parameter '%s'.\n", argvk[arg_idx + 2]);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          pc.dependency_flags |= kfFilterPvarReq | kfFilterNoSplitChr;
        } else if (strequal_k_unsafe(flagname_p2, "et-all-var-ids") || strequal_k_unsafe(flagname_p2, "et-missing-var-ids")) {
          if (flagname_p2[3] == 'm') {
            if (unlikely(pc.varid_template_str)) {
              logerrputs("Error: --set-missing-var-ids cannot be used with --set-all-var-ids.\n");
              goto main_ret_INVALID_CMDLINE;
            }
            pc.misc_flags |= kfMiscSetMissingVarIds;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (unlikely(!VaridTemplateIsValid(argvk[arg_idx + 1], flagname_p))) {
            goto main_ret_INVALID_CMDLINE_A;
          }
          reterr = CmdlineAllocString(argvk[arg_idx + 1], argvk[arg_idx], kMaxIdSlen, &pc.varid_template_str);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.dependency_flags |= kfFilterPvarReq;
        } else if (strequal_k_unsafe(flagname_p2, "et-hh-missing")) {
          if (unlikely(!(pc.command_flags1 & kfCommand1MakePlink2))) {
            logerrputs("Error: --set-hh-missing must be used with --make-[b]pgen/--make-bed.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (param_ct) {
            const char* cur_modif = argvk[arg_idx + 1];
            if (likely(!strcmp(cur_modif, "keep-dosage"))) {
              if (unlikely(make_plink2_flags & kfMakePgenEraseDosage)) {
                logerrputs("Error: --set-hh-missing 'keep-dosage' modifier cannot be used with\n--make-[b]pgen erase-dosage.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              make_plink2_flags |= kfMakePlink2SetHhMissingKeepDosage;
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --set-hh-missing parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          make_plink2_flags |= kfMakePlink2SetHhMissing;
        } else if (strequal_k_unsafe(flagname_p2, "et-mixed-mt-missing")) {
          if (unlikely(!(pc.command_flags1 & kfCommand1MakePlink2))) {
            logerrputs("Error: --set-mixed-mt-missing must be used with --make-[b]pgen/--make-bed.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (param_ct) {
            const char* cur_modif = argvk[arg_idx + 1];
            if (likely(!strcmp(cur_modif, "keep-dosage"))) {
              if (unlikely(make_plink2_flags & kfMakePgenEraseDosage)) {
                logerrputs("Error: --set-mixed-mt-missing 'keep-dosage' modifier cannot be used with\n--make-[b]pgen erase-dosage.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              make_plink2_flags |= kfMakePlink2SetMixedMtMissingKeepDosage;
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --set-mixed-mt-missing parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          make_plink2_flags |= kfMakePlink2SetMixedMtMissing;
        } else if (strequal_k_unsafe(flagname_p2, "ample")) {
          if (unlikely(load_params || (xload & (~(kfXloadOxGen | kfXloadOxBgen | kfXloadOxHaps | kfXloadOxLegend | kfXloadOxSample))))) {
            goto main_ret_INVALID_CMDLINE_INPUT_CONFLICT;
          }
          if (unlikely(!(xload & (kfXloadOxGen | kfXloadOxBgen | kfXloadOxHaps)))) {
            logerrputs("Error: --sample must be used with --gen/--bgen/--data/--haps.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_fname = argvk[arg_idx + 1];
          const uint32_t slen = strlen(cur_fname);
          if (unlikely(slen > kPglFnamesize - 1)) {
            logerrputs("Error: --sample filename too long.\n");
            goto main_ret_OPEN_FAIL;
          }
          memcpy(psamname, cur_fname, slen + 1);
          xload |= kfXloadOxSample;
        } else if (strequal_k_unsafe(flagname_p2, "heep")) {
          if (unlikely(chr_info.chrset_source)) {
            logerrputs("Error: Conflicting chromosome-set flags.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          chr_info.chrset_source = kChrsetSourceCmdline;
          chr_info.autosome_ct = 26;
          chr_info.xymt_codes[0] = 27;
          chr_info.xymt_codes[1] = 28;
          chr_info.xymt_codes[2] = UINT32_MAXM1;
          chr_info.xymt_codes[3] = UINT32_MAXM1;
          chr_info.xymt_codes[4] = UINT32_MAXM1;
          chr_info.xymt_codes[5] = UINT32_MAXM1;
          chr_info.haploid_mask[0] = 0x18000000;
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "np")) {
          if (unlikely(pc.varid_exclude_snp)) {
            // problematic due to --window
            logerrputs("Error: --snp cannot be used with --exclude-snp.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = CmdlineAllocString(argvk[arg_idx + 1], argvk[arg_idx], kMaxIdSlen, &pc.varid_snp);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.filter_flags |= kfFilterPvarReq;
        } else if (strequal_k_unsafe(flagname_p2, "nps")) {
          reterr = ParseNameRanges(&(argvk[arg_idx]), errstr_append, param_ct, 0, range_delim, &pc.snps_range_list);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.filter_flags |= kfFilterPvarReq;
        } else if (strequal_k_unsafe(flagname_p2, "nps-only")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (param_ct) {
            const char* cur_modif = argvk[arg_idx + 1];
            if (likely(!strcmp(cur_modif, "just-acgt"))) {
              pc.filter_flags |= kfFilterSnpsOnlyJustAcgt;
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --snps-only parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          pc.filter_flags |= kfFilterPvarReq | kfFilterSnpsOnly;
        } else if (strequal_k_unsafe(flagname_p2, "core")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 12))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocFname(argvk[arg_idx + 1], flagname_p, 0, &pc.score_info.input_fname);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          uint32_t numeric_param_ct = 0;
          uint32_t score_cols[4];
          for (uint32_t param_idx = 2; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "header", cur_modif_slen)) {
              pc.score_info.flags |= kfScoreHeaderIgnore;
            } else if (strequal_k(cur_modif, "header-read", cur_modif_slen)) {
              pc.score_info.flags |= kfScoreHeaderRead;
            } else if (strequal_k(cur_modif, "no-mean-imputation", cur_modif_slen)) {
              pc.score_info.flags |= kfScoreNoMeanimpute;
            } else if (strequal_k(cur_modif, "dominant", cur_modif_slen)) {
              pc.score_info.flags |= kfScoreDominant;
            } else if (strequal_k(cur_modif, "recessive", cur_modif_slen)) {
              pc.score_info.flags |= kfScoreRecessive;
            } else if (strequal_k(cur_modif, "center", cur_modif_slen)) {
              pc.score_info.flags |= kfScoreCenter;
            } else if (strequal_k(cur_modif, "variance-standardize", cur_modif_slen)) {
              pc.score_info.flags |= kfScoreVarianceStandardize;
            } else if (strequal_k(cur_modif, "variance-normalize", cur_modif_slen)) {
              logerrputs("Note: --score's 'variance-normalize' modifier has been renamed to the more\nprecise 'variance-standardize'.\n");
              pc.score_info.flags |= kfScoreVarianceStandardize;
            } else if (strequal_k(cur_modif, "se", cur_modif_slen)) {
              pc.score_info.flags |= kfScoreSe;
            } else if (strequal_k(cur_modif, "zs", cur_modif_slen)) {
              pc.score_info.flags |= kfScoreZs;
            } else if (strequal_k(cur_modif, "ignore-dup-ids", cur_modif_slen)) {
              pc.score_info.flags |= kfScoreIgnoreDupIds;
            } else if (strequal_k(cur_modif, "list-variants", cur_modif_slen)) {
              pc.score_info.flags |= kfScoreListVariants;
            } else if (strequal_k(cur_modif, "list-variants-zs", cur_modif_slen)) {
              pc.score_info.flags |= kfScoreListVariants | kfScoreListVariantsZs;
            } else if (StrStartsWith(cur_modif, "cols=", cur_modif_slen)) {
              if (unlikely(pc.score_info.flags & kfScoreColAll)) {
                logerrputs("Error: Multiple --score cols= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              // TODO (alpha 3): nmissallele -> nallele
              reterr = ParseColDescriptor(&(cur_modif[5]), "maybefid\0fid\0maybesid\0sid\0pheno1\0phenos\0nmissallele\0denom\0dosagesum\0scoreavgs\0scoresums\0", "score", kfScoreColMaybefid, kfScoreColDefault, 1, &pc.score_info.flags);
              if (unlikely(reterr)) {
                goto main_ret_1;
              }
            } else {
              if (unlikely(ScanPosintCappedx(cur_modif, kMaxLongLine / 2, &(score_cols[numeric_param_ct])))) {
                snprintf(g_logbuf, kLogbufSize, "Error: Invalid --score parameter '%s'.\n", cur_modif);
                goto main_ret_INVALID_CMDLINE_WWA;
              }
              if (unlikely(numeric_param_ct == 3)) {
                logerrputs("Error: --score takes at most three numeric parameters.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              for (uint32_t uii = 0; uii != numeric_param_ct; ++uii) {
                if (unlikely(score_cols[uii] == score_cols[numeric_param_ct])) {
                  logerrputs("Error: Identical --score column indexes.\n");
                  goto main_ret_INVALID_CMDLINE_A;
                }
              }
              ++numeric_param_ct;
            }
          }
          if (unlikely((pc.score_info.flags & (kfScoreHeaderIgnore | kfScoreHeaderRead)) == (kfScoreHeaderIgnore | kfScoreHeaderRead))) {
            logerrputs("Error: --score 'header' and 'header-read' modifiers cannot be used together.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          uint32_t model_flags_u = S_CAST(uint32_t, pc.score_info.flags & (kfScoreDominant | kfScoreRecessive | kfScoreCenter | kfScoreVarianceStandardize));
          if (unlikely(model_flags_u & (model_flags_u - 1))) {
            logerrputs("Error: --score 'dominant', 'recessive', 'center', and 'variance-standardize'\nmodifiers are mutually exclusive.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (!(pc.score_info.flags & kfScoreColAll)) {
            pc.score_info.flags |= kfScoreColDefault;
          }
          if (numeric_param_ct) {
            pc.score_info.varid_col_p1 = score_cols[0];
          }
          if (numeric_param_ct > 1) {
            pc.score_info.allele_col_p1 = score_cols[1];
          } else {
            pc.score_info.allele_col_p1 = pc.score_info.varid_col_p1 + 1;
          }
          if (numeric_param_ct == 3) {
            // a bit artificial, but it works
            const uint32_t col_idx = score_cols[2];
            const uint32_t col_idx_blen = 1 + UintSlen(col_idx);
            char* new_buf;
            if (unlikely(pgl_malloc(col_idx_blen + 1, &new_buf))) {
              goto main_ret_NOMEM;
            }
            pc.score_info.input_col_idx_range_list.names = new_buf;
            pc.score_info.input_col_idx_range_list.name_max_blen = col_idx_blen;
            pc.score_info.input_col_idx_range_list.name_ct = 1;
            u32toa_x(col_idx, '\0', new_buf);
            new_buf[col_idx_blen] = '\0';
            pc.score_info.input_col_idx_range_list.starts_range = R_CAST(unsigned char*, &(new_buf[col_idx_blen]));
          }
          pc.command_flags1 |= kfCommand1Score;
          pc.dependency_flags |= kfFilterAllReq;
        } else if (strequal_k_unsafe(flagname_p2, "core-col-nums")) {
          if (unlikely(!(pc.command_flags1 & kfCommand1Score))) {
            logerrputs("Error: --score-col-nums must be used with --score.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(pc.score_info.input_col_idx_range_list.name_ct)) {
            logerrputs("Error: --score-col-nums cannot be used when three numeric parameters are\nprovided to --score.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          reterr = ParseNameRanges(&(argvk[arg_idx]), errstr_append, param_ct, 1, '-', &pc.score_info.input_col_idx_range_list);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
        } else if (strequal_k_unsafe(flagname_p2, "plit-cat-pheno")) {
          uint32_t first_phenoname_idx = 1;
          for (; first_phenoname_idx <= param_ct; ++first_phenoname_idx) {
            const char* cur_modif = argvk[arg_idx + first_phenoname_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "omit-most", cur_modif_slen)) {
              pc.pheno_transform_flags |= kfPhenoTransformSplitCatOmitMost;
            } else if (strequal_k(cur_modif, "omit-last", cur_modif_slen)) {
              pc.pheno_transform_flags |= kfPhenoTransformSplitCatOmitLast;
            } else if (strequal_k(cur_modif, "covar-01", cur_modif_slen)) {
              pc.pheno_transform_flags |= kfPhenoTransformSplitCatCovar01;
            } else {
              break;
            }
          }
          if ((pc.pheno_transform_flags & (kfPhenoTransformSplitCatOmitMost | kfPhenoTransformSplitCatOmitLast)) == (kfPhenoTransformSplitCatOmitMost | kfPhenoTransformSplitCatOmitLast)) {
            logerrputs("Error: --split-cat-pheno 'omit-most' and 'omit-last' modifiers cannot be used\ntogether.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (first_phenoname_idx <= param_ct) {
            reterr = AllocAndFlatten(&(argvk[arg_idx + first_phenoname_idx]), param_ct + 1 - first_phenoname_idx, kMaxIdSlen - 1, &pc.split_cat_phenonames_flattened);
            if (unlikely(reterr)) {
              goto main_ret_1;
            }
            // may as well verify that no phenotype name has an '=' in it
            const char* phenonames_iter = pc.split_cat_phenonames_flattened;
            do {
              const uint32_t cur_phenoname_slen = strlen(phenonames_iter);
              if (unlikely(memchr(phenonames_iter, '=', cur_phenoname_slen))) {
                logerrputs("Error: --split-cat-pheno phenotype names may not contain the '=' character.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              phenonames_iter = &(phenonames_iter[cur_phenoname_slen + 1]);
            } while (*phenonames_iter);
          } else if (unlikely(pc.pheno_transform_flags & kfPhenoTransformSplitCatCovar01)) {
            logerrputs("Error: --split-cat-pheno 'covar-01' modifier cannot be used without any\nphenotype names.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          pc.pheno_transform_flags |= kfPhenoTransformSplitCat;
          pc.dependency_flags |= kfFilterPsamReq;
        } else if (strequal_k_unsafe(flagname_p2, "ort-vars")) {
          if (unlikely(!(pc.command_flags1 & kfCommand1MakePlink2))) {
            // todo: permit merge
            logerrputs("Error: --sort-vars must be used with --make-[b]pgen/--make-bed or dataset\nmerging.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (param_ct) {
            const char* mode_str = argvk[arg_idx + 1];
            const char first_char_upcase_match = mode_str[0] & 0xdf;
            const uint32_t mode_slen = strlen(mode_str);
            if (((mode_slen == 1) && (first_char_upcase_match == 'N')) ||
                strequal_k(mode_str, "natural", mode_slen)) {
              pc.sort_vars_flags = kfSortNatural;
            } else if (likely(
                ((mode_slen == 1) && (first_char_upcase_match == 'A')) ||
                strequal_k(mode_str, "ascii", mode_slen))) {
              pc.sort_vars_flags = kfSortAscii;
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: '%s' is not a valid mode for --sort-vars.\n", mode_str);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          } else {
            pc.sort_vars_flags = kfSortNatural;
          }
        } else if (strequal_k_unsafe(flagname_p2, "ample-diff")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 0x7fffffff))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          uint32_t param_idx = 1;

          // defer this since default depends on whether we're in 'pairwise'
          // mode
          uint32_t diff_cols_param_idx = 0;
          uint32_t is_file = 0;
          char sdiff_id_delim = '\0';
          for (; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (StrStartsWith(cur_modif, "id-delim=", cur_modif_slen)) {
              if (unlikely(sdiff_id_delim)) {
                logerrputs("Error: Multiple --sample-diff id-delim= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              sdiff_id_delim = ExtractCharParam(&(cur_modif[strlen("id-delim=")]));
              if (unlikely(!sdiff_id_delim)) {
                logerrputs("Error: --sample-diff id-delim= value must be a single character.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              if (unlikely((ctou32(sdiff_id_delim) <= ' ') || (sdiff_id_delim == '.'))) {
                logerrputs("Error: --sample-diff id-delim= value cannot be tab/space/newline, '.', or a\nnonprinting character.\n");
                goto main_ret_INVALID_CMDLINE;
              }
            } else if (StrStartsWith(cur_modif, "dosage", cur_modif_slen)) {
              if (unlikely(pc.sdiff_info.dosage_hap_tol != kDosageMissing)) {
                logerrputs("Error: Multiple --sample-diff dosage modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              if (cur_modif[6] == '\0') {
                pc.sdiff_info.dosage_hap_tol = 0;
              } else if (cur_modif[6] != '=') {
                snprintf(g_logbuf, kLogbufSize, "Error: Invalid --sample-diff parameter '%s'.\n", cur_modif);
                goto main_ret_INVALID_CMDLINE_WWA;
              } else {
                double dxx;
                if (unlikely((!ScantokDouble(&(cur_modif[7]), &dxx)) || (dxx < 0.0) || (dxx > (0.5 - kSmallEpsilon)))) {
                  snprintf(g_logbuf, kLogbufSize, "Error: Invalid --sample-diff parameter '%s'.\n", cur_modif);
                  goto main_ret_INVALID_CMDLINE_WWA;
                }
                pc.sdiff_info.dosage_hap_tol = S_CAST(int32_t, dxx * ((1 + kSmallEpsilon) * kDosageMax));
              }
            } else if (strequal_k(cur_modif, "include-missing", cur_modif_slen)) {
              pc.sdiff_info.flags |= kfSdiffIncludeMissing;
            } else if (strequal_k(cur_modif, "pairwise", cur_modif_slen)) {
              pc.sdiff_info.flags |= kfSdiffPairwise;
            } else if (strequal_k(cur_modif, "counts-only", cur_modif_slen)) {
              pc.sdiff_info.flags |= kfSdiffCountsOnly;
            } else if (StrStartsWith(cur_modif, "fname-id-delim=", cur_modif_slen)) {
              if (unlikely(pc.sdiff_info.fname_id_delim)) {
                logerrputs("Error: Multiple --sample-diff fname-id-delim= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              pc.sdiff_info.fname_id_delim = ExtractCharParam(&(cur_modif[strlen("fname-id-delim=")]));
              if (unlikely(!pc.sdiff_info.fname_id_delim)) {
                logerrputs("Error: --sample-diff fname-id-delim= value must be a single character.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
              if (unlikely((ctou32(pc.sdiff_info.fname_id_delim) <= ' ') || (pc.sdiff_info.fname_id_delim == '.'))) {
                logerrputs("Error: --sample-diff fname-id-delim= value cannot be tab/space/newline, '.', or\na nonprinting character.\n");
                goto main_ret_INVALID_CMDLINE;
              }
            } else if (strequal_k(cur_modif, "zs", cur_modif_slen)) {
              pc.sdiff_info.flags |= kfSdiffZs;
            } else if (StrStartsWith0(cur_modif, "cols=", cur_modif_slen)) {
              if (unlikely(diff_cols_param_idx)) {
                logerrputs("Error: Multiple --sample-diff cols= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              diff_cols_param_idx = param_idx;
            } else if (StrStartsWith(cur_modif, "counts-cols=", cur_modif_slen)) {
              if (unlikely(pc.sdiff_info.flags & kfSdiffCountsColAll)) {
                logerrputs("Error: Multiple --sample-diff counts-cols= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              reterr = ParseColDescriptor(&(cur_modif[strlen("counts-cols=")]), "maybefid\0fid\0maybesid\0sid\0nobs\0nobsibs\0ibs0\0ibs1\0ibs2\0halfmiss\0diff\0", "sample-diff counts-cols=", kfSdiffCountsColMaybefid, kfSdiffCountsColDefault, 1, &pc.sdiff_info.flags);
              if (unlikely(reterr)) {
                goto main_ret_1;
              }
            } else if (StrStartsWith(cur_modif, "base=", cur_modif_slen)) {
              reterr = CmdlineAllocString(&(cur_modif[5]), argvk[arg_idx], kPglFnamesize - 11, &(pc.sdiff_info.first_id_or_fname));
              if (unlikely(reterr)) {
                goto main_ret_1;
              }
              pc.sdiff_info.flags |= kfSdiffOneBase;
              break;
            } else if (StrStartsWith(cur_modif, "ids=", cur_modif_slen)) {
              reterr = CmdlineAllocString(&(cur_modif[4]), argvk[arg_idx], kMaxIdSlen, &(pc.sdiff_info.first_id_or_fname));
              if (unlikely(reterr)) {
                goto main_ret_1;
              }
              break;
            } else if (likely(StrStartsWith(cur_modif, "file=", cur_modif_slen))) {
              reterr = AllocFname(&(cur_modif[5]), argvk[arg_idx], 0, &pc.sdiff_info.first_id_or_fname);
              if (unlikely(reterr)) {
                goto main_ret_1;
              }
              is_file = 1;
              break;
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --sample-diff parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          if (param_idx > param_ct) {
            logerrputs("Error: Invalid --sample-diff parameter sequence (base=/id= must be\nsecond-to-last parameter or earlier, or file= must be last parameter).\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (diff_cols_param_idx) {
            reterr = ParseColDescriptor(&(argvk[arg_idx + diff_cols_param_idx][5]), "chrom\0pos\0ref\0alt\0maybefid\0fid\0id\0maybesid\0sid\0geno\0", "sample-diff cols=", kfSdiffColChrom, (pc.sdiff_info.flags & kfSdiffPairwise)? kfSdiffColPairwiseDefault : kfSdiffColDefault, 0, &pc.sdiff_info.flags);
            if (unlikely(reterr)) {
              goto main_ret_1;
            }
            if (!(pc.sdiff_info.flags & kfSdiffColId)) {
              if (unlikely(pc.sdiff_info.flags & (kfSdiffColMaybefid | kfSdiffColFid | kfSdiffColMaybesid | kfSdiffColSid))) {
                logerrputs("Error: Invalid --sample-diff cols= set ('maybefid', 'fid', 'maybesid', and\n'sid' require 'id').\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
            }
          } else {
            pc.sdiff_info.flags |= (pc.sdiff_info.flags & kfSdiffPairwise)? kfSdiffColPairwiseDefault : kfSdiffColDefault;
          }
          if ((pc.sdiff_info.flags & (kfSdiffPairwise | kfSdiffCountsOnly)) == (kfSdiffPairwise | kfSdiffCountsOnly)) {
            logerrputs("Error: --sample-diff 'pairwise' and 'counts-only' modifiers cannot be used\ntogether.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          const uint32_t other_id_ct = param_ct - param_idx;
          if (!other_id_ct) {
            if (unlikely(!is_file)) {
              logerrputs("Error: --sample-diff 'base='/'ids=' require 2 or more space-separated sample\nIDs.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
            if (sdiff_id_delim) {
              logerrputs("Error: --sample-diff id-delim= modifier does not apply to file= mode.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
          } else {
            if (unlikely(is_file)) {
              // this constraint is a bit arbitrary, but may as well have it
              // for consistency with base=/ids=
              logerrputs("Error: --sample-diff 'file=' must appear after all other modifiers.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
            reterr = AllocAndFlatten(&(argvk[arg_idx + param_idx + 1]), other_id_ct, kPglFnamesize, &pc.sdiff_info.other_ids_flattened);
            if (unlikely(reterr)) {
              goto main_ret_1;
            }
            if (sdiff_id_delim) {
              char* id_iter = pc.sdiff_info.first_id_or_fname;
              if (unlikely(ReplaceCharAdvChecked(sdiff_id_delim, ' ', &id_iter))) {
                logerrputs("Error: --sample-diff sample IDs cannot include spaces.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              id_iter = pc.sdiff_info.other_ids_flattened;
              for (uint32_t uii = 0; uii != other_id_ct; ++uii) {
                if (unlikely(ReplaceCharAdvChecked(sdiff_id_delim, ' ', &id_iter))) {
                  logerrputs("Error: --sample-diff sample IDs cannot include spaces.\n");
                  goto main_ret_INVALID_CMDLINE;
                }
                ++id_iter;
              }
            }
          }
          pc.sdiff_info.other_id_ct = other_id_ct;
          if (!(pc.sdiff_info.flags & kfSdiffCountsColAll)) {
            pc.sdiff_info.flags |= kfSdiffCountsColDefault;
          }
          if ((pc.sdiff_info.flags & (kfSdiffIncludeMissing | kfSdiffCountsColHalfmiss)) == kfSdiffCountsColHalfmiss) {
            pc.sdiff_info.flags &= ~kfSdiffCountsColHalfmiss;
          }
          if (!pc.sdiff_info.fname_id_delim) {
            pc.sdiff_info.fname_id_delim = '_';
          }
          pc.command_flags1 |= kfCommand1Sdiff;
          pc.dependency_flags |= kfFilterAllReq;
        } else if (strequal_k_unsafe(flagname_p2, "ample-counts")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          for (uint32_t param_idx = 1; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "zs", cur_modif_slen)) {
              pc.sample_counts_flags |= kfSampleCountsZs;
            } else if (likely(StrStartsWith(cur_modif, "cols=", cur_modif_slen))) {
              if (unlikely(pc.sample_counts_flags & kfGenoCountsColAll)) {
                logerrputs("Error: Multiple --sample-counts cols= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              reterr = ParseColDescriptor(&(cur_modif[5]), "maybefid\0fid\0maybesid\0sid\0sex\0hom\0homref\0homalt\0homaltsnp\0het\0refalt\0het2alt\0hetsnp\0dipts\0ts\0diptv\0tv\0dipnonsnpsymb\0nonsnpsymb\0symbolic\0nonsnp\0dipsingle\0single\0haprefwfemaley\0hapref\0hapaltwfemaley\0hapalt\0missingwithfemaley\0missing\0", "sample-counts", kfSampleCountsColMaybefid, kfSampleCountsColDefault, 1, &pc.sample_counts_flags);
              if (unlikely(reterr)) {
                goto main_ret_1;
              }
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --sample-counts parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          if (!(pc.sample_counts_flags & kfSampleCountsColAll)) {
            pc.sample_counts_flags |= kfSampleCountsColDefault;
          }
          pc.command_flags1 |= kfCommand1SampleCounts;
          pc.dependency_flags |= kfFilterAllReq;
        } else if (strequal_k_unsafe(flagname_p2, "trict-sid0")) {
          pc.misc_flags |= kfMiscStrictSid0;
          goto main_param_zero;
        } else if (unlikely(!strequal_k_unsafe(flagname_p2, "ilent"))) {
          goto main_ret_INVALID_CMDLINE_UNRECOGNIZED;
        }
        break;

      case 't':
        if (strequal_k_unsafe(flagname_p2, "hreads")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (unlikely(ScanPosintDefcapx(argvk[arg_idx + 1], &pc.max_thread_ct))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --threads parameter '%s'.\n", argvk[arg_idx + 1]);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          if (pc.max_thread_ct > kMaxThreads) {
            logprintf("Note: Reducing --threads parameter to %u.  (If this is not large enough,\nrecompile with a larger kMaxThreads setting.)\n", kMaxThreads);
            pc.max_thread_ct = kMaxThreads;
          } else if (known_procs == -1) {
            // trigger BLAS/LAPACK warning?
            known_procs = 0;
          }
        } else if (strequal_k_unsafe(flagname_p2, "o")) {
          if (unlikely(chr_info.is_include_stack || notchr_present)) {
            logerrputs("Error: --from/--to cannot be used with --autosome{-par} or --{not-}chr.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = CmdlineAllocString(argvk[arg_idx + 1], argvk[arg_idx], kMaxIdSlen, &pc.varid_to);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.filter_flags |= kfFilterPvarReq | kfFilterNoSplitChr;
        } else if (strequal_k_unsafe(flagname_p2, "o-bp") || strequal_k_unsafe(flagname_p2, "o-kb") || strequal_k_unsafe(flagname_p2, "o-mb")) {
          if (unlikely(!CmdlineSingleChr(&chr_info, pc.misc_flags))) {
            logerrputs("Error: --from-bp/-kb/-mb and --to-bp/-kb/-mb must be used with --chr, and only\none chromosome.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(notchr_present)) {
            logerrputs("Error: --from-bp/-kb/-mb and --to-bp/-kb/-mb cannot be used with --not-chr.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(pc.to_bp != -1)) {
            logerrputs("Error: Multiple --to-bp/-kb/-mb values.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          double dxx;
          if (unlikely(!ScantokDouble(cur_modif, &dxx))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --to-bp/-kb/-mb parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          const char unit_char = flagname_p2[2];
          if (unit_char == 'k') {
            dxx *= 1000;
          } else if (unit_char == 'm') {
            dxx *= 1000000;
          }
          if (unlikely(dxx < 0)) {
            logerrprintf("Error: --to-bp/-kb/-mb parameter '%s' too small.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_A;
          } else if (dxx >= 2147483646) {
            pc.to_bp = 0x7ffffffe;
          } else {
            // round down
            pc.to_bp = S_CAST(int32_t, dxx * (1 + kSmallEpsilon));
          }
          if (unlikely(pc.from_bp > pc.to_bp)) {
            // (if we do permit this, rounding must be postponed)
            logerrputs("Error: --to-bp/-kb/-mb parameter is smaller than --from-bp/-kb/-mb parameter.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          pc.filter_flags |= kfFilterPvarReq;
        } else if (strequal_k_unsafe(flagname_p2, "hin")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          if (unlikely(!ScantokDouble(cur_modif, &pc.thin_keep_prob))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --thin variant retention probability '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          if (unlikely(pc.thin_keep_prob < (0.5 / 4294967296.0))) {
            logerrputs("Error: --thin variant retention probability too small.\n");
            goto main_ret_INVALID_CMDLINE_A;
          } else if (unlikely(pc.thin_keep_prob >= (4294967295.5 / 4294967296.0))) {
            uint32_t uii;
            if (ScanUintDefcapx(cur_modif, &uii)) {
              logerrputs("Error: --thin variant retention probability too large.\n");
            } else {
              // VCFtools --thin = --bp-space...
              logerrputs("Error: --thin variant retention probability too large.  (Did you mean\n--bp-space?)\n");
            }
            goto main_ret_INVALID_CMDLINE_A;
          }
          pc.filter_flags |= kfFilterPvarReq;
        } else if (strequal_k_unsafe(flagname_p2, "hin-count")) {
          if (unlikely(pc.thin_keep_prob != 1.0)) {
            logerrputs("Error: --thin cannot be used with --thin-count.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          if (unlikely(ScanUintDefcapx(cur_modif, &pc.thin_keep_ct) || (!pc.thin_keep_ct))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --thin-count parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          pc.filter_flags |= kfFilterPvarReq;
        } else if (strequal_k_unsafe(flagname_p2, "hin-indiv")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          if (unlikely(!ScantokDouble(cur_modif, &pc.thin_keep_sample_prob))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --thin-indiv sample retention probability '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          if (unlikely(pc.thin_keep_sample_prob < (0.5 / 4294967296.0))) {
            logerrputs("Error: --thin-indiv sample retention probability too small.\n");
            goto main_ret_INVALID_CMDLINE_A;
          } else if (unlikely(pc.thin_keep_sample_prob >= (4294967295.5 / 4294967296.0))) {
            logerrputs("Error: --thin-indiv sample retention probability too large.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          pc.filter_flags |= kfFilterPsamReq;
        } else if (strequal_k_unsafe(flagname_p2, "hin-indiv-count")) {
          if (unlikely(pc.thin_keep_sample_prob != 1.0)) {
            logerrputs("Error: --thin-indiv cannot be used with --thin-indiv-count.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          if (unlikely(ScanUintDefcapx(cur_modif, &pc.thin_keep_sample_ct) || (!pc.thin_keep_sample_ct))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --thin-indiv-count parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          pc.filter_flags |= kfFilterPsamReq;
        } else if (likely(strequal_k_unsafe(flagname_p2, "ests"))) {
          if (unlikely(!(pc.command_flags1 & kfCommand1Glm))) {
            logerrputs("Error: --tests must be used with --glm.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if ((param_ct == 1) && (!strcmp(argvk[arg_idx + 1], "all"))) {
            pc.glm_info.flags |= kfGlmTestsAll;
          } else {
            reterr = ParseNameRanges(&(argvk[arg_idx]), errstr_append, param_ct, 1, '-', &pc.glm_info.tests_range_list);
            if (unlikely(reterr)) {
              goto main_ret_1;
            }
          }
        } else {
          goto main_ret_INVALID_CMDLINE_UNRECOGNIZED;
        }
        break;

      case 'u':
        if (strequal_k_unsafe(flagname_p2, "pdate-sex")) {
          if (unlikely(pc.update_sample_ids_fname)) {
            logerrputs("Error: --update-sex cannot be used with --update-ids.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 4))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocFname(argvk[arg_idx + 1], flagname_p, 0, &pc.update_sex_info.fname);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          for (uint32_t param_idx = 2; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "male0", cur_modif_slen)) {
              pc.update_sex_info.flags |= kfUpdateSexMale0;
            } else if (StrStartsWith(cur_modif, "col-num=", cur_modif_slen)) {
              const char* col_num_start = &(cur_modif[strlen("col-num=")]);
              if (unlikely(ScanPosintDefcapx(col_num_start, &pc.update_sex_info.col_num) || (pc.update_sex_info.col_num == 1))) {
                snprintf(g_logbuf, kLogbufSize, "Error: Invalid --update-sex col-num= parameter '%s'.\n", col_num_start);
                goto main_ret_INVALID_CMDLINE_WWA;
              }
            } else if (likely(param_ct == 2)) {
              // only one extra parameter, try to interpret it the plink 1.9
              // way but print a warning
              if (unlikely(ScanPosintDefcapx(cur_modif, &pc.update_sex_info.col_num))) {
                snprintf(g_logbuf, kLogbufSize, "Error: Invalid --update-sex parameter '%s'.\n", cur_modif);
                goto main_ret_INVALID_CMDLINE_WWA;
              }
              logerrputs("Warning: --update-sex unlabeled column parameter is now deprecated.  Use\n'col-num=' instead (and add 2 to the value).\n");
              pc.update_sex_info.col_num += 2;
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --update-sex parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          pc.dependency_flags |= kfFilterPsamReq;
        } else if (strequal_k_unsafe(flagname_p2, "pdate-map")) {
          if (unlikely(pc.recover_var_ids_fname)) {
            logerrputs("Error: --update-map cannot be used with --recover-var-ids or --update-name.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 4))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = Alloc2col(&(argvk[arg_idx + 1]), flagname_p, param_ct, &pc.update_map_flag);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.dependency_flags |= kfFilterPvarReq;
        } else if (strequal_k_unsafe(flagname_p2, "pdate-name")) {
          if (unlikely(pc.recover_var_ids_fname)) {
            logerrputs("Error: --update-name cannot be used with --recover-var-ids.\n");
          }
          if (unlikely(pc.update_map_flag)) {
            logerrputs("Error: --update-map cannot be used with --recover-var-ids or --update-name.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 4))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = Alloc2col(&(argvk[arg_idx + 1]), flagname_p, param_ct, &pc.update_name_flag);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.dependency_flags |= kfFilterPvarReq;
        } else if (strequal_k_unsafe(flagname_p2, "pdate-ids")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocFname(argvk[arg_idx + 1], flagname_p, 0, &pc.update_sample_ids_fname);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.dependency_flags |= kfFilterPsamReq;
        } else if (strequal_k_unsafe(flagname_p2, "pdate-parents")) {
          if (unlikely(pc.update_sample_ids_fname)) {
            logerrputs("Error: --update-parents cannot be used with --update-ids.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocFname(argvk[arg_idx + 1], flagname_p, 0, &pc.update_parental_ids_fname);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.dependency_flags |= kfFilterPsamReq;
        } else if (likely(strequal_k_unsafe(flagname_p2, "pdate-alleles"))) {
          // Prevent the most confusing order-of-operations dependencies.  (May
          // want to prevent more combinations later.)
          if (unlikely(pc.varid_template_str)) {
            logerrputs("Error: --update-alleles cannot be used with --set-missing-var-ids or\n--set-all-var-ids.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocFname(argvk[arg_idx + 1], flagname_p, 0, &pc.update_alleles_fname);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          pc.dependency_flags |= kfFilterPvarReq;
        } else {
          goto main_ret_INVALID_CMDLINE_UNRECOGNIZED;
        }
        break;

      case 'v':
        if (strequal_k_unsafe(flagname_p2, "ar-min-qual")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (unlikely(ScanFloat(argvk[arg_idx + 1], &pc.var_min_qual) || (pc.var_min_qual < S_CAST(float, 0.0)))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --var-min-qual parameter '%s'.\n", argvk[arg_idx + 1]);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          pc.var_min_qual *= S_CAST(float, 1 - kSmallEpsilon);
          pc.filter_flags |= kfFilterPvarReq;
        } else if (strequal_k_unsafe(flagname_p2, "ar-filter")) {
          if (param_ct) {
            reterr = AllocAndFlatten(&(argvk[arg_idx + 1]), param_ct, 0x7fffffff, &pc.var_filter_exceptions_flattened);
            if (unlikely(reterr)) {
              goto main_ret_1;
            }
          }
          pc.misc_flags |= kfMiscExcludePvarFilterFail;
          pc.filter_flags |= kfFilterPvarReq;
        } else if (strequal_k_unsafe(flagname_p2, "cf")) {
          // permit accompanying .fam/.psam
          // IIDs must match VCF sample line order
          if (unlikely((load_params & (~kfLoadParamsPsam)) || xload)) {
            goto main_ret_INVALID_CMDLINE_INPUT_CONFLICT;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (param_ct == 2) {
            const char* cur_modif = argvk[arg_idx + 2];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            // tolerate vcf-dosage= as well, so it's possible to use the same
            // pattern as --export
            if (unlikely((!StrStartsWith(cur_modif, "dosage=", cur_modif_slen)) && (!StrStartsWith(cur_modif, "vcf-dosage=", cur_modif_slen)))) {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --vcf parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
            const char* dosage_field_start = (cur_modif[0] == 'v')? &(cur_modif[11]) : &(cur_modif[7]);
            reterr = CmdlineAllocString(dosage_field_start, argvk[arg_idx], 4095, &vcf_dosage_import_field);
            if (unlikely(reterr)) {
              goto main_ret_1;
            }
            if (unlikely(!((!strcmp(vcf_dosage_import_field, "GP-force")) || IsAlphanumeric(vcf_dosage_import_field)))) {
              logerrputs("Error: --vcf dosage= parameter is not alphanumeric.\n");
              goto main_ret_INVALID_CMDLINE;
            }
            if (unlikely(!strcmp(vcf_dosage_import_field, "GT"))) {
              logerrputs("Error: --vcf dosage= parameter cannot be 'GT'.\n");
              goto main_ret_INVALID_CMDLINE;
            }
          }
          const char* cur_modif = argvk[arg_idx + 1];
          const uint32_t slen = strlen(cur_modif);
          if (unlikely(slen > kPglFnamesize - 1)) {
            logerrputs("Error: --vcf filename too long.\n");
            goto main_ret_OPEN_FAIL;
          }
          memcpy(pgenname, cur_modif, slen + 1);
          xload = kfXloadVcf;
        } else if (unlikely(strequal_k_unsafe(flagname_p2, "cf-min-gp"))) {
          logerrputs("Error: --vcf-min-gp is no longer supported.  Use --import-dosage-certainty\ninstead.\n");
          goto main_ret_INVALID_CMDLINE_A;
        } else if (strequal_k_unsafe(flagname_p2, "cf-min-gq") || strequal_k_unsafe(flagname_p2, "cf-min-dp")) {
          if (unlikely(!(xload & kfXloadVcf))) {
            // todo: support BCF too
            logerrprintf("Error: --%s must be used with --vcf.\n", flagname_p);
            goto main_ret_INVALID_CMDLINE;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          uint32_t uii;
          if (unlikely(ScanUintDefcapx(cur_modif, &uii))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --%s parameter '%s'.\n", flagname_p, cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          if (flagname_p2[7] == 'g') {
            vcf_min_gq = uii;
          } else {
            vcf_min_dp = uii;
            if (vcf_max_dp < vcf_min_dp) {
              logerrputs("Error: --vcf-min-dp value cannot be larger than --vcf-max-dp value.\n");
              goto main_ret_INVALID_CMDLINE;
            }
          }
        } else if (strequal_k_unsafe(flagname_p2, "cf-max-dp")) {
          if (unlikely(!(xload & kfXloadVcf))) {
            logerrputs("Error: --vcf-max-dp must be used with --vcf/--bcf.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          uint32_t uii;
          if (unlikely(ScanUintDefcapx(cur_modif, &uii))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --vcf-max-dp parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          vcf_max_dp = uii;
        } else if (strequal_k_unsafe(flagname_p2, "cf-idspace-to")) {
          if (unlikely(!(xload & (kfXloadVcf | kfXloadBcf)))) {
            logerrputs("Error: --vcf-idspace-to must be used with --vcf/--bcf.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          if (unlikely(id_delim == ' ')) {
            logerrputs("Error: --vcf-idspace-to cannot be used when the --id-delim character is space.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          idspace_to = ExtractCharParam(argvk[arg_idx + 1]);
          if (unlikely(!idspace_to)) {
            logerrputs("Error: --vcf-idspace-to parameter must be a single character.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(ctou32(idspace_to) <= ' ')) {
            logerrputs("Error: --vcf-idspace-to parameter must be a nonspace character.\n");
            goto main_ret_INVALID_CMDLINE;
          }
        } else if (strequal_k_unsafe(flagname_p2, "cf-half-call")) {
          if (unlikely(!(xload & kfXloadVcf))) {
            logerrputs("Error: --vcf-half-call must be used with --vcf.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* mode_str = argvk[arg_idx + 1];
          const char first_char_upcase_match = mode_str[0] & 0xdf;
          const uint32_t mode_slen = strlen(mode_str);
          if (((mode_slen == 1) && (first_char_upcase_match == 'H')) ||
              strequal_k(mode_str, "haploid", mode_slen)) {
            vcf_half_call = kVcfHalfCallHaploid;
          } else if (((mode_slen == 1) && (first_char_upcase_match == 'M')) ||
                     strequal_k(mode_str, "missing", mode_slen)) {
            vcf_half_call = kVcfHalfCallMissing;
          } else if (((mode_slen == 1) && (first_char_upcase_match == 'E')) ||
                     strequal_k(mode_str, "error", mode_slen)) {
            vcf_half_call = kVcfHalfCallError;
          } else if (likely(
              ((mode_slen == 1) && (first_char_upcase_match == 'R')) ||
              strequal_k(mode_str, "reference", mode_slen))) {
            vcf_half_call = kVcfHalfCallError;
          } else {
            snprintf(g_logbuf, kLogbufSize, "Error: '%s' is not a valid mode for --vcf-half-call.\n", mode_str);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
        } else if (strequal_k_unsafe(flagname_p2, "cf-require-gt")) {
          if (unlikely(!(xload & (kfXloadVcf | kfXloadBcf)))) {
            logerrputs("Error: --vcf-require-gt must be used with --vcf/--bcf.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          import_flags |= kfImportVcfRequireGt;
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "if")) {
          if (unlikely(!(pc.command_flags1 & kfCommand1Glm))) {
            logerrputs("Error: --vif must be used with --glm/--epistasis.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          if (unlikely(!ScantokDouble(cur_modif, &pc.vif_thresh))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --glm/--epistasis VIF threshold '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          if (unlikely(pc.vif_thresh < 1.0)) {
            snprintf(g_logbuf, kLogbufSize, "Error: --glm/--epistasis VIF threshold '%s' too small (must be >= 1).\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
        } else if (strequal_k_unsafe(flagname_p2, "ariance-standardize")) {
          if (unlikely(pc.pheno_transform_flags & kfPhenoTransformVstdCovar)) {
            logerrputs("Error: --variance-standardize cannot be used with --covar-variance-standardize.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          if (param_ct) {
            reterr = AllocAndFlatten(&(argvk[arg_idx + 1]), param_ct, 0x7fffffff, &pc.vstd_flattened);
            if (unlikely(reterr)) {
              goto main_ret_1;
            }
          }
          pc.pheno_transform_flags |= kfPhenoTransformVstdAll;
          pc.dependency_flags |= kfFilterPsamReq;
        } else if (strequal_k_unsafe(flagname_p2, "ar-id-multi") ||
                   strequal_k_unsafe(flagname_p2, "ar-id-multi-nonsnp")) {
          if (unlikely(!pc.varid_template_str)) {
            logerrputs("Error: --var-id-multi{-nonsnp} must be used with --set-missing-var-ids or\n--set-all-var-ids.\n");
            goto main_ret_INVALID_CMDLINE;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (unlikely(!VaridTemplateIsValid(argvk[arg_idx + 1], flagname_p))) {
            goto main_ret_INVALID_CMDLINE_A;
          }
          reterr = CmdlineAllocString(argvk[arg_idx + 1], argvk[arg_idx], kMaxIdSlen, flagname_p2[11]? (&pc.varid_multi_nonsnp_template_str) : (&pc.varid_multi_template_str));
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
        } else if (strequal_k_unsafe(flagname_p2, "ariant-score")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 3))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          reterr = AllocFname(argvk[arg_idx + 1], flagname_p, 0, &pc.vscore_fname);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
          uint32_t explicit_cols = 0;
          for (uint32_t param_idx = 2; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (strequal_k(cur_modif, "zs", cur_modif_slen)) {
              pc.vscore_flags |= kfVscoreZs;
            } else if (strequal_k(cur_modif, "bin", cur_modif_slen)) {
              // 'zs' and 'bin' *are* allowed together, since the variant-ID
              // (.vscore.vars) file accompanying the .vscore.bin can be large
              // enough to deserve compression.
              pc.vscore_flags |= kfVscoreBin;
            } else if (likely(StrStartsWith(cur_modif, "cols=", cur_modif_slen))) {
              if (unlikely(explicit_cols)) {
                logerrputs("Error: Multiple --variant-score cols= modifiers.\n");
                goto main_ret_INVALID_CMDLINE;
              }
              explicit_cols = 1;
              reterr = ParseColDescriptor(&(cur_modif[5]), "chrom\0pos\0ref\0alt1\0alt\0altfreq\0nmiss\0nobs\0", "variant-score", kfVscoreColChrom, kfVscoreColDefault, 0, &pc.vscore_flags);
              if (unlikely(reterr)) {
                goto main_ret_1;
              }
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --variant-score parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          }
          if (!explicit_cols) {
            pc.vscore_flags |= kfVscoreColDefault;
          } else if (pc.vscore_flags & kfVscoreBin) {
            logerrputs("Error: --variant-score 'bin' and 'cols=' modifiers cannot be used together.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          pc.command_flags1 |= kfCommand1Vscore;
          pc.dependency_flags |= kfFilterAllReq;
        } else if (strequal_k_unsafe(flagname_p2, "score-col-nums")) {
          if (unlikely(!(pc.command_flags1 & kfCommand1Vscore))) {
            logerrputs("Error: --vscore-col-nums must be used with --variant-score.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          reterr = ParseNameRanges(&(argvk[arg_idx]), errstr_append, param_ct, 1, '-', &pc.vscore_col_idx_range_list);
          if (unlikely(reterr)) {
            goto main_ret_1;
          }
        } else if (likely(strequal_k_unsafe(flagname_p2, "alidate"))) {
          pc.command_flags1 |= kfCommand1Validate;
          pc.dependency_flags |= kfFilterAllReq;
          goto main_param_zero;
        } else {
          goto main_ret_INVALID_CMDLINE_UNRECOGNIZED;
        }
        break;

      case 'w':
        if (strequal_k_unsafe(flagname_p2, "rite-snplist")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (param_ct) {
            const char* cur_modif = argvk[arg_idx + 1];
            if (unlikely(strcmp(cur_modif, "zs"))) {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --write-snplist parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
            pc.misc_flags |= kfMiscWriteSnplistZs;
          }
          pc.command_flags1 |= kfCommand1WriteSnplist;
          pc.dependency_flags |= kfFilterPvarReq;
        } else if (strequal_k_unsafe(flagname_p2, "rite-samples")) {
          if (unlikely((param_ct == 1) && (!strcmp(argvk[arg_idx + 1], "noheader")))) {
            logerrputs("Error: --write-samples 'noheader' modifier retired.  Use --no-id-header\ninstead.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          pc.command_flags1 |= kfCommand1WriteSamples;
          pc.dependency_flags |= kfFilterPsamReq;
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "indow")) {
          if (unlikely(!(pc.varid_snp || pc.varid_exclude_snp))) {
            logerrputs("Error: --window must be used with --snp or --exclude-snp.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          double dxx;
          if (unlikely((!ScantokDouble(cur_modif, &dxx)) || (dxx < 0))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --window parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
          dxx *= 500 * (1 + kSmallEpsilon);
          if (dxx > 2147483646) {
            pc.window_bp = 0x7ffffffe;
          } else {
            pc.window_bp = S_CAST(int32_t, dxx);
          }
          pc.filter_flags |= kfFilterNoSplitChr;
          // no need to set kfFilterPvarReq due to --snp/--exclude-snp req.
        } else if (strequal_k_unsafe(flagname_p2, "ithin")) {
          if (unlikely(pc.misc_flags & kfMiscCatPhenoFamily)) {
            logerrputs("Error: --within cannot be used with --family.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 2))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          for (uint32_t param_idx = 1; param_idx <= param_ct; ++param_idx) {
            const char* cur_modif = argvk[arg_idx + param_idx];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (unlikely((cur_modif_slen == 7) && StrStartsWithUnsafe(cur_modif, "keep-") && MatchUpperK(&(cur_modif[5]), "NA"))) {
              logerrputs("Error: --within's keep-NA modifier has been retired.  Rename that category in\nthe input file if you wish to keep it.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
            if (param_idx == 1) {
              reterr = AllocFname(cur_modif, flagname_p, 0, &pc.within_fname);
            } else {
              if (unlikely(IsReservedPhenoName(cur_modif, cur_modif_slen))) {
                snprintf(g_logbuf, kLogbufSize, "Error: '%s' cannot be used as a categorical phenotype name.\n", cur_modif);
                goto main_ret_INVALID_CMDLINE_2A;
              }
              reterr = CmdlineAllocString(cur_modif, argvk[arg_idx], kMaxIdSlen, &pc.catpheno_name);
            }
            if (unlikely(reterr)) {
              goto main_ret_1;
            }
          }
          pc.dependency_flags |= kfFilterPsamReq;
        } else if (strequal_k_unsafe(flagname_p2, "rite-covar")) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 0, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (param_ct) {
            const char* cur_modif = argvk[arg_idx + 1];
            const uint32_t cur_modif_slen = strlen(cur_modif);
            if (likely(StrStartsWith0(cur_modif, "cols=", cur_modif_slen))) {
              reterr = ParseColDescriptor(&(cur_modif[5]), "maybefid\0fid\0maybesid\0sid\0maybeparents\0parents\0sex\0pheno1\0phenos\0", "write-covar", kfWriteCovarColMaybefid, kfWriteCovarColDefault, 0, &pc.write_covar_flags);
              if (unlikely(reterr)) {
                goto main_ret_1;
              }
            } else {
              snprintf(g_logbuf, kLogbufSize, "Error: Invalid --write-covar parameter '%s'.\n", cur_modif);
              goto main_ret_INVALID_CMDLINE_WWA;
            }
          } else {
            pc.write_covar_flags |= kfWriteCovarColDefault;
          }
          pc.command_flags1 |= kfCommand1WriteCovar;
          pc.dependency_flags |= kfFilterPsamReq;
        } else if (likely(strequal_k_unsafe(flagname_p2, "arning-errcode"))) {
          warning_errcode = 1;
          goto main_param_zero;
        } else if (strequal_k_unsafe(flagname_p2, "rite-cluster")) {
          logerrputs("Error: --write-cluster is retired.  Use e.g. --make-just-psam.\n");
          goto main_ret_INVALID_CMDLINE_A;
        } else if (strequal_k_unsafe(flagname_p2, "ith-phenotype")) {
          logerrputs("Error: --with-phenotype is retired.  Use --write-covar cols=... instead.\n");
          goto main_ret_INVALID_CMDLINE_A;
        } else {
          goto main_ret_INVALID_CMDLINE_UNRECOGNIZED;
        }
        break;

      case 'x':
        if (likely(strequal_k_unsafe(flagname_p2, "chr-model"))) {
          if (unlikely(!(pc.command_flags1 & (kfCommand1Glm | kfCommand1Score | kfCommand1Vscore)))) {
            logerrputs("Error: --xchr-model must be used with --glm, --score, or --variant-score.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
          if (unlikely(pc.glm_info.flags & (kfGlmGenotypic | kfGlmHethom | kfGlmDominant | kfGlmRecessive))) {
            snprintf(g_logbuf, kLogbufSize, "Error: --xchr-model cannot be used with --glm %s.\n", (pc.glm_info.flags & kfGlmGenotypic)? "genotypic" : ((pc.glm_info.flags & kfGlmHethom)? "hethom" : ((pc.glm_info.flags & kfGlmDominant)? "dominant" : "recessive")));
            goto main_ret_INVALID_CMDLINE_2A;
          }
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          pc.xchr_model = ctou32(ExtractCharParam(cur_modif)) - 48;
          if (unlikely(pc.xchr_model > 2)) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --xchr-model parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
        } else {
          goto main_ret_INVALID_CMDLINE_UNRECOGNIZED;
        }
        break;

      case 'z':
        if (likely(strequal_k_unsafe(flagname_p2, "st-level"))) {
          if (unlikely(EnforceParamCtRange(argvk[arg_idx], param_ct, 1, 1))) {
            goto main_ret_INVALID_CMDLINE_2A;
          }
          const char* cur_modif = argvk[arg_idx + 1];
          // Note that, in zstd 1.3.4, multithreaded compression is
          // nondeterministic unless level 5+ is explicitly requested, see
          //   https://github.com/facebook/zstd/issues/1077 .
          // I've postponed the decision on whether this sort of nondeterminism
          // is acceptable in plink2 for now (by reverting to 1.3.3).
          if (unlikely(ScanPosintCappedx(cur_modif, 22, &g_zst_level))) {
            snprintf(g_logbuf, kLogbufSize, "Error: Invalid --zst-level parameter '%s'.\n", cur_modif);
            goto main_ret_INVALID_CMDLINE_WWA;
          }
        } else {
          goto main_ret_INVALID_CMDLINE_UNRECOGNIZED;
        }
        break;

      default:
        goto main_ret_INVALID_CMDLINE_UNRECOGNIZED;
      main_param_zero:
        if (unlikely(param_ct)) {
          snprintf(g_logbuf, kLogbufSize, "Error: --%s doesn't accept parameters.\n", flagname_p);
          goto main_ret_INVALID_CMDLINE_2A;
        }
      }
    } while ((++cur_flag_idx) < flag_ct);
    if (!outname_end) {
      outname_end = &(outname[6]);
    }

    pc.dependency_flags |= pc.filter_flags;
    const uint32_t skip_main = (!pc.command_flags1) && (!(xload & (kfXloadVcf | kfXloadBcf | kfXloadOxBgen | kfXloadOxHaps | kfXloadOxSample | kfXloadPlink1Dosage | kfXloadGenDummy)));
    const uint32_t batch_job = (adjust_file_info.fname != nullptr);
    if (skip_main && (!batch_job)) {
      // add command_flags2 when needed
      goto main_ret_NULL_CALC;
    }
    if (unlikely(!(load_params || xload || batch_job))) {
      logerrputs("Error: No input dataset.\n");
      goto main_ret_INVALID_CMDLINE_A;
    }
    if (unlikely((xload & kfXloadOxGen) && (!(xload & kfXloadOxSample)))) {
      // could permit .fam/.psam, but unless Oxford software supports that mode
      // it's pointless
      logerrputs("Error: --gen must be used with --sample or --data.\n");
      goto main_ret_INVALID_CMDLINE_A;
    }
    if (unlikely((xload & kfXloadOxSample) && (pc.misc_flags & kfMiscAffection01))) {
      // necessary for --data and --data --make-pgen to yield the same output
      logerrputs("Error: --data/--sample cannot be used with --1.\n");
      goto main_ret_INVALID_CMDLINE_A;
    }
    if (unlikely((pc.sample_sort_flags != kfSort0) && (!(pc.command_flags1 & (kfCommand1MakePlink2 | kfCommand1WriteCovar))))) {
      // todo: permit merge
      logerrputs("Error: --indiv-sort must be used with --make-[b]pgen/--make-bed/--write-covar\nor dataset merging.\n");
      goto main_ret_INVALID_CMDLINE_A;
    }
    if (unlikely((make_plink2_flags & (kfMakePlink2MMask | kfMakePlink2TrimAlts | kfMakePlink2EraseAlt2Plus | kfMakePgenErasePhase | kfMakePgenEraseDosage)) && (pc.command_flags1 & (~kfCommand1MakePlink2)))) {
      logerrputs("Error: When the 'multiallelics=', 'trim-alts', and/or 'erase-...' modifier is\npresent, --make-bed/--make-[b]pgen cannot be combined with other commands.\n(Other filters are fine.)\n");
      goto main_ret_INVALID_CMDLINE;
    }
    if (make_plink2_flags & kfMakePlink2MMask) {
      if (unlikely((pc.misc_flags & kfMiscMajRef) || pc.ref_allele_flag || pc.alt1_allele_flag || (pc.fa_flags & kfFaRefFrom))) {
        logerrputs("Error: --make-bed/--make-[b]pgen 'multiallelics=' cannot be used with\n'trim-alts'.\n");
        goto main_ret_INVALID_CMDLINE;
      }
      if (unlikely((pc.misc_flags & kfMiscMajRef) || pc.ref_allele_flag || pc.alt1_allele_flag || (pc.fa_flags & kfFaRefFrom))) {
        logerrputs("Error: When the 'multiallelics=' modifier is present, --make-bed/--make-[b]pgen\ncannot be used with a flag which alters REF/ALT1 allele settings.\n");
        goto main_ret_INVALID_CMDLINE;
      }
    }
    if (pc.command_flags1 & (~(kfCommand1MakePlink2 | kfCommand1Exportf))) {
      if (unlikely((pc.misc_flags & kfMiscMajRef) || pc.ref_allele_flag || pc.alt1_allele_flag || (pc.fa_flags & kfFaRefFrom))) {
        logerrputs("Error: Flags which alter REF/ALT1 allele settings (--maj-ref, --ref-allele,\n--alt1-allele, --ref-from-fa) must be used with\n--make-bed/--make-[b]pgen/--export and no other commands.\n");
        goto main_ret_INVALID_CMDLINE;
      }
      if (unlikely(pc.fa_flags & kfFaNormalize)) {
        logerrputs("Error: --normalize must be used with --make-bed/--make-[b]pgen/--export and no\nother commands.\n");
        goto main_ret_INVALID_CMDLINE;
      }
    }
    if (pc.keep_cat_phenoname && (!pc.keep_cat_names_flattened) && (!pc.keep_cats_fname)) {
      logerrputs("Error: --keep-cat-pheno must be used with --keep-cats and/or --keep-cat-names.\n");
    }
    if (pc.remove_cat_phenoname && (!pc.remove_cat_names_flattened) && (!pc.remove_cats_fname)) {
      logerrputs("Error: --remove-cat-pheno must be used with --remove-cats and/or\n--remove-cat-names.\n");
    }
    if (aperm_present && (pc.command_flags1 & kfCommand1Glm) && (!(pc.glm_info.flags & kfGlmPerm))) {
      // If --aperm is present, at least one association analysis command which
      // supports adaptive permutation testing was also specified, but no
      // actual adaptive permutation test is happening, the user is likely to
      // be confused.  Produce a warning.  (Not an error since a sophisticated
      // user may want to use --script with different --aperm defaults.)
      logerrputs("Warning: --aperm only controls the settings for adaptive permutation tests; it\ndoes not cause such a test to be performed.  (Did you forget to add the 'perm'\nmodifier to an association analysis flag?)\n");
    }
    if ((pc.hard_call_thresh == UINT32_MAX) && (xload & (kfXloadVcf | kfXloadBcf | kfXloadOxGen | kfXloadOxBgen))) {
      if (unlikely(pc.dosage_erase_thresh > (kDosageMid / 10))) {
        logerrputs("Error: --dosage-erase-threshold value cannot be larger than (default)\n--hard-call-threshold value.\n");
        goto main_ret_INVALID_CMDLINE_A;
      }
    } else {
      if (unlikely(pc.dosage_erase_thresh > pc.hard_call_thresh)) {
        logerrputs("Error: --dosage-erase-threshold value cannot be larger than\n--hard-call-threshold value.\n");
        goto main_ret_INVALID_CMDLINE_A;
      }
    }
    if (unlikely((oxford_import_flags & (kfOxfordImportRefFirst | kfOxfordImportRefLast | kfOxfordImportRefUnknown)) == (kfOxfordImportRefFirst | kfOxfordImportRefLast | kfOxfordImportRefUnknown))) {
      logerrputs("Error: --data/--{b}gen 'ref-first', 'ref-last', and 'ref-unknown' modifiers\ncannot be used together.\n");
      goto main_ret_INVALID_CMDLINE;
    }
    if (unlikely(!strcmp(g_missing_catname, g_output_missing_pheno))) {
      logerrputs("Error: --missing-catname and --output-missing-phenotype strings can't match.\n");
      goto main_ret_INVALID_CMDLINE_A;
    }
    if (unlikely((pc.misc_flags & kfMiscChrOverrideCmdline) && (!chr_info.chrset_source))) {
      logerrputs("Error: --chr-override requires an explicit chromosome set.\n");
      goto main_ret_INVALID_CMDLINE_A;
    }
    if (unlikely((xload & kfXloadPlink1Dosage) && (!(load_params & kfLoadParamsPsam)))) {
      logerrputs("Error: --import-dosage requires a .fam file.\n");
      goto main_ret_INVALID_CMDLINE_A;
    }
    if (unlikely((pc.misc_flags & kfMiscCovarColNums) && (!pc.covar_fname) && (!pc.pheno_fname))) {
      logerrputs("Error: --covar-col-nums requires --covar or --pheno.\n");
      goto main_ret_INVALID_CMDLINE_A;
    }
    if ((pc.grm_flags & kfGrmMatrixShapemask) && (pc.misc_flags & kfMiscNoIdHeader)) {
      pc.grm_flags |= kfGrmNoIdHeader;
    }
    if (unlikely(pc.score_info.qsr_range_fname && (!pc.score_info.input_fname))) {
      logerrputs("Error: --q-score-range cannot be used without --score.\n");
      goto main_ret_INVALID_CMDLINE_A;
    }
    if (pc.extract_fcol_info.params) {
      if (unlikely((!pc.extract_fcol_info.match_substr) && pc.extract_fcol_info.match_flattened && pc.extract_fcol_info.mismatch_flattened)) {
        logerrputs("Error: --extract-fcol-match and --extract-fcol-mismatch can only be used\ntogether when --extract-fcol-substr is specified.\n");
        goto main_ret_INVALID_CMDLINE_A;
      }
      if (unlikely(pc.extract_fcol_info.max < pc.extract_fcol_info.min)) {
        logerrputs("Error: --extract-fcol-max value can't be smaller than --extract-fcol-min value.\n");
        goto main_ret_INVALID_CMDLINE_A;
      }
    }
    if (!permit_multiple_inclusion_filters) {
      // Permit only one position- or ID-based variant inclusion filter, since
      // it's not immediately obvious whether the union or intersection should
      // be taken with multiple inclusion filters.
      // However, multiple exclusion filters are fine.  (Also,
      // --autosome{-par}/--chr is exempted since it's more obvious how they
      // interact with other filters.)
      const uint32_t inclusion_filter_extract = (pc.extract_fnames != nullptr);
      const uint32_t inclusion_filter_extract_fcol = (pc.extract_fcol_info.params != nullptr);
      const uint32_t inclusion_filter_extract_intersect = (pc.extract_intersect_fnames != nullptr);
      const uint32_t inclusion_filter_fromto_id = pc.varid_from || pc.varid_to;
      const uint32_t inclusion_filter_fromto_bp = (pc.from_bp != -1) || (pc.to_bp != -1);
      const uint32_t inclusion_filter_snpflag = (pc.varid_snp != nullptr);
      const uint32_t inclusion_filter_snpsflag = !!pc.snps_range_list.name_ct;
      if (unlikely(inclusion_filter_extract + inclusion_filter_extract_fcol + inclusion_filter_extract_intersect + inclusion_filter_fromto_id + inclusion_filter_fromto_bp + inclusion_filter_snpflag + inclusion_filter_snpsflag > 1)) {
        logerrputs("Error: Multiple variant inclusion filters specified (--extract, --extract-fcol,\n--extract-intersect, --from/--to, --from-bp/--to-bp, --snp, --snps).  Add\n--force-intersect if you really want the intersection of these sets.  (If your\nvariant IDs are unique, you can extract the union by e.g. running\n--write-snplist for each set, followed by --extract on all the .snplist files.)\n");
        goto main_ret_INVALID_CMDLINE_A;
      }
    }

    sfmt_t main_sfmt;
    if (!rseeds) {
      uint32_t seed = S_CAST(uint32_t, time(nullptr));
      snprintf(g_logbuf, kLogbufSize, "Random number seed: %u\n", seed);
      logputs_silent(g_logbuf);
      sfmt_init_gen_rand(&main_sfmt, seed);
    } else {
      if (rseed_ct == 1) {
        sfmt_init_gen_rand(&main_sfmt, rseeds[0]);
      } else {
        sfmt_init_by_array(&main_sfmt, rseeds, rseed_ct);
      }
      free(rseeds);
      rseeds = nullptr;
    }

    if (unlikely(CmdlineParsePhase3(0, malloc_size_mib, memory_require, &pcm, &bigstack_ua))) {
      goto main_ret_NOMEM;
    }
    g_input_missing_geno_ptr = &(g_one_char_strs[2 * ctou32(input_missing_geno_char)]);
    g_output_missing_geno_ptr = &(g_one_char_strs[2 * ctou32(output_missing_geno_char)]);
    // pigz_init(pc.max_thread_ct);

    if (pc.max_thread_ct > 8) {
      logprintf("Using up to %u threads (change this with --threads).\n", pc.max_thread_ct);
    } else {
      // "1 compute thread" instead of "1 thread" since, when
      // max_thread_ct == 2, some code will use one I/O thread and one
      // compute thread.  Not worth the trouble of writing special-case code
      // to avoid that.  (also, with 2 cores, the I/O thread isn't
      // sufficiently busy to justify only 1 compute thread.)
      logprintf("Using %s%u compute thread%s.\n", (pc.max_thread_ct > 1)? "up to " : "", pc.max_thread_ct, (pc.max_thread_ct == 1)? "" : "s");
    }
    if (randmem) {
      reterr = RandomizeBigstack(pc.max_thread_ct, &main_sfmt);
      if (unlikely(reterr)) {
        goto main_ret_1;
      }
    }

    print_end_time = 1;

    if (batch_job) {
      if (adjust_file_info.fname) {
        reterr = AdjustFile(&adjust_file_info, pc.ln_pfilter, pc.output_min_ln, pc.max_thread_ct, outname, outname_end);
        if (unlikely(reterr)) {
          goto main_ret_1;
        }
      }
      if (skip_main) {
        goto main_ret_1;
      }
    }

    // nonstandard cases (CNV, etc.) here
    if ((pc.command_flags1 == kfCommand1PgenInfo) && (load_params != kfLoadParamsPfileAll) && (!xload)) {
      // special case: don't require .psam/.pvar file
      if (unlikely(!(load_params & kfLoadParamsPgen))) {
        logerrputs("Error: --pgen-info requires a .pgen file.\n");
        goto main_ret_INVALID_CMDLINE;
      }
      reterr = PgenInfoStandalone(pgenname);
    } else {
      if (unlikely(pc.dependency_flags && (!pc.command_flags1))) {
        logerrputs("Error: Basic file conversions do not support regular filter or transform\noperations.  Rerun your command with --make-bed/--make-[b]pgen.\n");
        goto main_ret_INVALID_CMDLINE;
      }
      if (xload) {
        char* convname_end = outname_end;
        if (pc.command_flags1) {
          if (import_flags & kfImportKeepAutoconv) {
            if (unlikely(pc.misc_flags & kfMiscAffection01)) {
              logerrputs("Error: --1 cannot be used with --keep-autoconv.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
            if (unlikely((output_missing_geno_char != '.') && (output_missing_geno_char != input_missing_geno_char))) {
              logerrputs("Error: --output-missing-genotype and --input-missing-genotype parameters cannot\nbe inconsistent when --keep-autoconv is specified.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
            double dxx;
            const char* num_end = ScantokDouble(g_output_missing_pheno, &dxx);
            if (num_end) {
              if (unlikely(dxx != S_CAST(double, pc.missing_pheno))) {
                logerrputs("Error: --output-missing-phenotype and --input-missing-phenotype parameters\ncannot be inconsistent when --keep-autoconv is specified.\n");
                goto main_ret_INVALID_CMDLINE_A;
              }
            } else if (unlikely(!IsNanStr(g_output_missing_pheno, strlen(g_output_missing_pheno)))) {
              logerrputs("Error: --output-missing-phenotype parameter must be numeric or 'NA' when\n--keep-autoconv is specified.\n");
              goto main_ret_INVALID_CMDLINE_A;
            }
          } else {
            convname_end = Stpcpy(convname_end, "-temporary");
          }
        } else {
          import_flags |= kfImportKeepAutoconv;
        }
        const uint32_t convname_slen = convname_end - outname;
        uint32_t pgen_generated = 1;
        uint32_t psam_generated = 1;
        if (xload & kfXloadVcf) {
          const uint32_t no_samples_ok = !(pc.dependency_flags & (kfFilterAllReq | kfFilterPsamReq));
          if (no_samples_ok && (!(import_flags & kfImportKeepAutoconv)) && pc.command_flags1) {
            // special case: just treat the VCF as a .pvar file
            strcpy(pvarname, pgenname);
            pgenname[0] = '\0';
            goto main_reinterpret_vcf_instead_of_converting;
          } else {
            reterr = VcfToPgen(pgenname, (load_params & kfLoadParamsPsam)? psamname : nullptr, const_fid, vcf_dosage_import_field, pc.misc_flags, import_flags, no_samples_ok, pc.hard_call_thresh, pc.dosage_erase_thresh, import_dosage_certainty, id_delim, idspace_to, vcf_min_gq, vcf_min_dp, vcf_max_dp, vcf_half_call, pc.fam_cols, pc.max_thread_ct, outname, convname_end, &chr_info, &pgen_generated, &psam_generated);
          }
        } else if (xload & kfXloadBcf) {
          logerrputs("Error: --bcf is not implemented yet.\n");
          reterr = kPglRetNotYetSupported;
        } else if (xload & kfXloadOxGen) {
          reterr = OxGenToPgen(pgenname, psamname, import_single_chr_str, ox_missing_code, pc.misc_flags, import_flags, oxford_import_flags, pc.hard_call_thresh, pc.dosage_erase_thresh, import_dosage_certainty, pc.max_thread_ct, outname, convname_end, &chr_info);
        } else if (xload & kfXloadOxBgen) {
          reterr = OxBgenToPgen(pgenname, psamname, const_fid, import_single_chr_str, ox_missing_code, pc.misc_flags, import_flags, oxford_import_flags, pc.hard_call_thresh, pc.dosage_erase_thresh, import_dosage_certainty, id_delim, idspace_to, pc.max_thread_ct, outname, convname_end, &chr_info);
        } else if (xload & kfXloadOxHaps) {
          reterr = OxHapslegendToPgen(pgenname, pvarname, psamname, import_single_chr_str, ox_missing_code, pc.misc_flags, import_flags, oxford_import_flags, pc.max_thread_ct, outname, convname_end, &chr_info);
        } else if (xload & kfXloadPlink1Dosage) {
          reterr = Plink1DosageToPgen(pgenname, psamname, (xload & kfXloadMap)? pvarname : nullptr, import_single_chr_str, &plink1_dosage_info, pc.misc_flags, import_flags, pc.fam_cols, pc.missing_pheno, pc.hard_call_thresh, pc.dosage_erase_thresh, import_dosage_certainty, pc.max_thread_ct, outname, convname_end, &chr_info);
        } else if (xload & kfXloadGenDummy) {
          reterr = GenerateDummy(&gendummy_info, pc.misc_flags, import_flags, pc.hard_call_thresh, pc.dosage_erase_thresh, pc.max_thread_ct, &main_sfmt, outname, convname_end, &chr_info);
        }
        if (reterr || (!pc.command_flags1)) {
          goto main_ret_1;
        }

        // todo: we have to skip this when merging is involved
        pc.hard_call_thresh = UINT32_MAX;
        pc.dosage_erase_thresh = 0;

        if (pgen_generated) {
          snprintf(memcpya(pgenname, outname, convname_slen), kMaxOutfnameExtBlen - 10, ".pgen");
        }
        snprintf(memcpya(pvarname, outname, convname_slen), kMaxOutfnameExtBlen - 10, ".pvar");
        if (psam_generated) {
          snprintf(memcpya(psamname, outname, convname_slen), kMaxOutfnameExtBlen - 10, ".psam");
        }
        if (!(import_flags & kfImportKeepAutoconv)) {
          if (pgen_generated) {
            if (unlikely(PushLlStr(pgenname, &file_delete_list))) {
              goto main_ret_NOMEM;
            }
          }
          if (unlikely(PushLlStr(pvarname, &file_delete_list))) {
            goto main_ret_NOMEM;
          }
          if (psam_generated) {
            if (unlikely(PushLlStr(psamname, &file_delete_list))) {
              goto main_ret_NOMEM;
            }
          }
        }
        *outname_end = '\0';
      }
    main_reinterpret_vcf_instead_of_converting:
      if ((pc.dependency_flags & kfFilterOpportunisticPgen) && (pgenname[0] != '\0')) {
        pc.dependency_flags |= kfFilterAllReq;
      }
      if (pc.dependency_flags & kfFilterAllReq) {
        if (unlikely((!xload) && (load_params != kfLoadParamsPfileAll))) {
          logerrputs("Error: A full fileset (.pgen/.bed + .pvar/.bim + .psam/.fam) is required for\nthis.\n");
          goto main_ret_INVALID_CMDLINE_A;
        }
      } else {
        // no genotype file required
        if (!(pc.dependency_flags & kfFilterOpportunisticPgen)) {
          pgenname[0] = '\0';
        }

        if (pc.dependency_flags & kfFilterPvarReq) {
          if (unlikely((!xload) && (!(load_params & kfLoadParamsPvar)))) {
            logerrputs("Error: A .pvar/.bim file is required for this.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
        } else {
          pvarname[0] = '\0';
        }
        if (pc.dependency_flags & kfFilterPsamReq) {
          if (unlikely((!xload) && (!(load_params & kfLoadParamsPsam)))) {
            logerrputs("Error: A .psam/.fam file is required for this.\n");
            goto main_ret_INVALID_CMDLINE_A;
          }
        } else {
          psamname[0] = '\0';
        }
      }
      if ((pc.command_flags1 & (~(kfCommand1MakePlink2 | kfCommand1Validate | kfCommand1WriteSnplist | kfCommand1WriteCovar | kfCommand1WriteSamples))) || ((pc.command_flags1 & kfCommand1MakePlink2) && (pc.sort_vars_flags == kfSort0))) {
        // split-chromosome prohibited for all commands unless explicitly
        // permitted here
        pc.dependency_flags |= kfFilterNoSplitChr;
      }

      BLAS_SET_NUM_THREADS(1);
      reterr = Plink2Core(&pc, make_plink2_flags, pgenname, psamname, pvarname, outname, outname_end, king_cutoff_fprefix, &chr_info, &main_sfmt);
    }
  }
  while (0) {
  main_ret_NOMEM:
    reterr = kPglRetNomem;
    break;
  main_ret_OPEN_FAIL:
    reterr = kPglRetOpenFail;
    break;
  main_ret_INVALID_CMDLINE_UNRECOGNIZED:
    InvalidArg(argv[arg_idx]);
    logerrputsb();
    logerrputs(errstr_append);
    reterr = kPglRetInvalidCmdline;
    break;
  main_ret_INVALID_CMDLINE_INPUT_CONFLICT:
    logerrprintf("Error: --%s conflicts with another input flag.\n%s", flagname_p, errstr_append);
    reterr = kPglRetInvalidCmdline;
    break;
  main_ret_INVALID_CMDLINE_WWA:
    WordWrapB(0);
  main_ret_INVALID_CMDLINE_2A:
    logerrputsb();
  main_ret_INVALID_CMDLINE_A:
    logerrputs(errstr_append);
  main_ret_INVALID_CMDLINE:
    reterr = kPglRetInvalidCmdline;
    break;
  main_ret_NULL_CALC:
    if (pc.dependency_flags) {
      logerrputs("Warning: No output requested.  (Did you forget --make-bed/--make-[b]pgen?)\nExiting.\n");
    } else {
      logerrputs("Warning: No output requested.  Exiting.\n");
    }
  main_ret_NULL_CALC_0:
    fputs(kCmdlineFormatStr, stdout);
    fputs(notestr_null_calc2, stdout);
    reterr = kPglRetSkipped;
    break;
  }
 main_ret_1:
  DispExitMsg(reterr);
  while (0) {
  main_ret_NOMEM_NOLOG:
    PrintVer();
  main_ret_NOMEM_NOLOG2:
    fputs(kErrstrNomem, stderr);
    if (g_failed_alloc_attempt_size) {
      fprintf(stderr, "Failed allocation size: %" PRIuPTR "\n", g_failed_alloc_attempt_size);
    }
    reterr = kPglRetNomem;
    break;
  }
 main_ret_NOLOG:
  free_cond(vcf_dosage_import_field);
  free_cond(ox_missing_code);
  free_cond(import_single_chr_str);
  free_cond(const_fid);
  free_cond(rseeds);
  CleanupPlink2CmdlineMeta(&pcm);
  CleanupAdjust(&adjust_file_info);
  free_cond(king_cutoff_fprefix);
  free_cond(pc.vscore_fname);
  free_cond(pc.recover_var_ids_fname);
  free_cond(pc.update_parental_ids_fname);
  free_cond(pc.update_sample_ids_fname);
  free_cond(pc.update_name_flag);
  free_cond(pc.update_map_flag);
  free_cond(pc.alt1_allele_flag);
  free_cond(pc.ref_allele_flag);
  free_cond(pc.update_alleles_fname);
  free_cond(pc.keep_fcol_name);
  free_cond(pc.keep_fcol_flattened);
  free_cond(pc.keep_fcol_fname);
  free_cond(pc.require_no_info_flattened);
  free_cond(pc.require_info_flattened);
  free_cond(pc.king_table_subset_fname);
  free_cond(pc.fa_fname);
  free_cond(pc.loop_cats_phenoname);
  free_cond(pc.covar_quantnorm_flattened);
  free_cond(pc.quantnorm_flattened);
  free_cond(pc.vstd_flattened);
  free_cond(pc.require_covar_flattened);
  free_cond(pc.require_pheno_flattened);
  free_cond(pc.split_cat_phenonames_flattened);
  free_cond(pc.remove_cat_phenoname);
  free_cond(pc.remove_cat_names_flattened);
  free_cond(pc.remove_cats_fname);
  free_cond(pc.keep_cat_phenoname);
  free_cond(pc.keep_cat_names_flattened);
  free_cond(pc.keep_cats_fname);
  free_cond(pc.family_missing_catname);
  free_cond(pc.catpheno_name);
  free_cond(pc.within_fname);
  free_cond(pc.read_freq_fname);
  free_cond(pc.glm_local_covar_fname);
  free_cond(pc.glm_local_pvar_fname);
  free_cond(pc.glm_local_psam_fname);
  free_cond(pc.freq_alt1_binstr);
  free_cond(pc.freq_ref_binstr);
  free_cond(pc.removefam_fnames);
  free_cond(pc.remove_fnames);
  free_cond(pc.keepfam_fnames);
  free_cond(pc.keep_fnames);
  free_cond(pc.exclude_fnames);
  free_cond(pc.extract_intersect_fnames);
  free_cond(pc.extract_fnames);
  free_cond(pc.sample_sort_fname);
  free_cond(pc.covar_fname);
  free_cond(pc.pheno_fname);
  free_cond(pc.varid_exclude_snp);
  free_cond(pc.varid_snp);
  free_cond(pc.varid_to);
  free_cond(pc.varid_from);
  free_cond(pc.missing_varid_match);
  free_cond(pc.varid_multi_nonsnp_template_str);
  free_cond(pc.varid_multi_template_str);
  free_cond(pc.varid_template_str);
  free_cond(pc.var_filter_exceptions_flattened);
  if (file_delete_list) {
    do {
      LlStr* llstr_ptr = file_delete_list->next;
      unlink(file_delete_list->str);
      free(file_delete_list);
      file_delete_list = llstr_ptr;
    } while (file_delete_list);
  }
  CleanupChrInfo(&chr_info);
  CleanupExportf(&pc.exportf_info);
  CleanupExtractFcol(&pc.extract_fcol_info);
  CleanupCmpExpr(&pc.exclude_if_info_expr);
  CleanupCmpExpr(&pc.extract_if_info_expr);
  CleanupCmpExpr(&pc.remove_if_expr);
  CleanupCmpExpr(&pc.keep_if_expr);
  CleanupScore(&pc.score_info);
  CleanupGlm(&pc.glm_info);
  CleanupSdiff(&pc.sdiff_info);
  CleanupLd(&pc.ld_info);
  CleanupUpdateSex(&pc.update_sex_info);
  CleanupRangeList(&pc.vscore_col_idx_range_list);
  CleanupRangeList(&pc.covar_range_list);
  CleanupRangeList(&pc.pheno_range_list);
  CleanupRangeList(&pc.exclude_snps_range_list);
  CleanupRangeList(&pc.snps_range_list);
  if (warning_errcode && g_stderr_written_to && (!reterr)) {
    logerrputs("--warning-errcode: One or more warnings in this run; exiting with code 61.\n");
    reterr = kPglRetWarningErrcode;
  }
  if (CleanupLogfile(print_end_time) && (!reterr)) {
    reterr = kPglRetWriteFail;
  }
  if (bigstack_ua) {
    free(bigstack_ua);
  }
  return S_CAST(int32_t, reterr);
}
