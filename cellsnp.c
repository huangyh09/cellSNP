
/* TODO: 
1. Try using multi_iter fetching method of bam/sam/cram for multi regions (SNPs) if it can in theory speed cellsnp up.
2. Consistency correction could be done in UMI groups with the help of @p pu & @p pl inside mplp structure.
3. More filters could be applied when extracting/fetching reads.
4. Improve the csp_fs_t structure, for example, adding @p is_error.
5. Improve the SZ_POOL structure, for example, adding @p base_init_f.
6. Use optional sparse matrices tags with the help of function pointers.
7. Output optionally qual values/letters to mtx file.
8. Deal with the problem that some UMIs have the letter 'N'.
 */
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <libgen.h>
#include <time.h>
#include <zlib.h>
#include <assert.h>
#include "thpool.h"
#include "general_util.h"
#include "cellsnp_util.h"

/* Define default values of global parameters. */
#define CSP_CHROM_ALL  {"1","2","3","4","5","6","7","8","9","10","11","12","13","14","15","16","17","18","19","20","21","22"}
#define CSP_NCHROM     22
#define CSP_CELL_TAG   "CB"
#define CSP_UMI_TAG    "UR"
#define CSP_NTHREAD    1
#define CSP_MIN_COUNT  20
#define CSP_MIN_MAF    0.0
#define CSP_MIN_LEN    30
#define CSP_MIN_MAPQ   20
#define CSP_MAX_FLAG   255
#define CSP_OUT_VCF_CELLS   "cellSNP.cells.vcf"
#define CSP_OUT_VCF_BASE    "cellSNP.base.vcf"
#define CSP_OUT_SAMPLES     "cellSNP.samples.tsv"
#define CSP_OUT_MTX_AD      "cellSNP.tag.AD.mtx"
#define CSP_OUT_MTX_DP      "cellSNP.tag.DP.mtx"
#define CSP_OUT_MTX_OTH     "cellSNP.tag.OTH.mtx"

/*Structure that stores global settings/options/parameters. 
Note:
1. In current version, one and only one of barcode(s) and sample-ID(s) would exist and work, the other
   would be freed. Refer to check_global_args() for details.
*/
struct _gll_settings {
    char *in_fn_file;      // Name of the file containing a list of input bam/sam/cram files, one input file per line.
    int nin;               // Num of input bam/sam/cram files.
    char **in_fns;         // Pointer to the array of names of input bam/sam/cram files.
    char *out_dir;         // Pointer to the path of dir containing the output files.   
    csp_fs_t *out_vcf_cells, *out_vcf_base, *out_samples;
    csp_fs_t *out_mtx_ad, *out_mtx_dp, *out_mtx_oth;
    int is_out_zip;        // If output files need to be zipped.
    int is_genotype;       // If need to do genotyping in addition to counting.
    char *pos_list_file;   // Name of file containing a list of SNPs, usually a vcf file.
    csp_snplist_t pl;      // List of the input SNPs. TODO: local variable.
    char *barcode_file;    // Name of the file containing a list of barcodes, one barcode per line.
    char **barcodes;       // Pointer to the array of barcodes.
    int nbarcode;          // Num of the barcodes.
    char *sid_list_file;   // Name of the file containing a list of sample IDs, one sample-ID per line.
    char **sample_ids;     // Pointer to the array of sample IDs.
    int nsid;              // Num of sample IDs.
    char **chrom_all;      // Pointer to the array of the chromosomes to use.
    int nchrom;            // Num of chromosomes.
    char *cell_tag;        // Tag for cell barcodes, NULL means no cell tags.
    char *umi_tag;         // Tag for UMI: UR, NULL. NULL means no UMI but read counts.
    int nthread;           // Num of threads.
    threadpool tp;         // Pointer to thread pool.
    int min_count;     // Minimum aggragated count.
    double min_maf;    // Minimum minor allele frequency.
    int double_gl;     // 0 or 1. 1: keep doublet GT likelihood, i.e., GT=0.5 and GT=1.5. 0: not keep.
    int min_len;       // Minimum mapped length for read filtering.
    int min_mapq;      // Minimum MAPQ for read filtering.
    int max_flag;      // Maximum FLAG for read filtering.
};

/*@abstract  Whether to use barcodes for sample grouping during pileup.
@param gs    Pointer of global settings structure [global_settings*].
@return      1, yes; 0, no.
*/
#define use_barcodes(gs) ((gs)->cell_tag)

/*@abstract  Whether to use sample IDs for sample grouping during pileup.
@param gs    Pointer of global settings structure [global_settings*].
@return      1, yes; 0, no.
*/
#define use_sid(gs) ((gs)->sample_ids)

/*@abstract  Whether to use UMI for reads grouping during pileup.
@param gs    Pointer of global settings structure [global_settings*].
@return      1, yes; 0, no.
*/
#define use_umi(gs) ((gs)->umi_tag)

/*@note  Do not free gs pointer itself! the system would do that! */
static void gll_setting_free(global_settings *gs) { 
    if (gs) {
        if (gs->in_fn_file) { free(gs->in_fn_file); gs->in_fn_file = NULL; }
        if (gs->in_fns) { str_arr_destroy(gs->in_fns, gs->nin); gs->in_fns = NULL; }
        if (gs->out_dir) { free(gs->out_dir); gs->out_dir = NULL; }
        if (gs->out_vcf_base) { csp_fs_destroy(gs->out_vcf_base); gs->out_vcf_base = NULL; }
        if (gs->out_vcf_cells) { csp_fs_destroy(gs->out_vcf_cells); gs->out_vcf_cells = NULL; } 
        if (gs->out_samples) { csp_fs_destroy(gs->out_samples); gs->out_samples = NULL; }    
        if (gs->out_mtx_ad) { csp_fs_destroy(gs->out_mtx_ad); gs->out_mtx_ad = NULL; }
        if (gs->out_mtx_dp) { csp_fs_destroy(gs->out_mtx_dp); gs->out_mtx_dp = NULL; }
        if (gs->out_mtx_oth) { csp_fs_destroy(gs->out_mtx_oth); gs->out_mtx_oth = NULL; } 
        if (gs->pos_list_file) { free(gs->pos_list_file); gs->pos_list_file = NULL; }
        csp_snplist_destroy(gs->pl);
        if (gs->barcode_file) { free(gs->barcode_file); gs->barcode_file = NULL; }
        if (gs->barcodes) { str_arr_destroy(gs->barcodes, gs->nbarcode); gs->barcodes = NULL; }
        if (gs->sid_list_file) { free(gs->sid_list_file); gs->sid_list_file = NULL; }
        if (gs->sample_ids) { str_arr_destroy(gs->sample_ids, gs->nsid); gs->sample_ids = NULL; }
        if (gs->chrom_all) { str_arr_destroy(gs->chrom_all, gs->nchrom); gs->chrom_all = NULL; }
        if (gs->cell_tag) { free(gs->cell_tag); gs->cell_tag = NULL; }
        if (gs->umi_tag) { free(gs->umi_tag); gs->umi_tag = NULL; }
        if (gs->tp) { thpool_destroy(gs->tp); gs->tp = NULL; }
    }
}

/*@abstract    Set default values for global_settings structure.
@param gs      Pointer to global_settings structure returned by gll_setting_init().
@return        Void.

@note          Internal use only!
 */
static void gll_set_default(global_settings *gs) {
    if (gs) {
        gs->in_fn_file = NULL; gs->in_fns = NULL; gs->nin = 0;
        gs->out_dir = NULL; 
        gs->out_vcf_base = NULL; gs->out_vcf_cells = NULL; gs->out_samples = NULL;
        gs->out_mtx_ad = NULL; gs->out_mtx_dp = NULL; gs->out_mtx_oth = NULL;
        gs->is_genotype = 0; gs->is_out_zip = 0;
        gs->pos_list_file = NULL; csp_snplist_init(gs->pl);
        gs->barcode_file = NULL; gs->nbarcode = 0; gs->barcodes = NULL; 
        gs->sid_list_file = NULL; gs->sample_ids = NULL; gs->nsid = 0;
        char *chrom_tmp[] = CSP_CHROM_ALL;
        gs->chrom_all = (char**) calloc(CSP_NCHROM, sizeof(char*));
        for (gs->nchrom = 0; gs->nchrom < CSP_NCHROM; gs->nchrom++) { gs->chrom_all[gs->nchrom] = safe_strdup(chrom_tmp[gs->nchrom]); }
        gs->cell_tag = safe_strdup(CSP_CELL_TAG); gs->umi_tag = safe_strdup(CSP_UMI_TAG);
        gs->nthread = CSP_NTHREAD; gs->tp = NULL;
        gs->min_count = CSP_MIN_COUNT; gs->min_maf = CSP_MIN_MAF; 
        gs->double_gl = 0;
        gs->min_len = CSP_MIN_LEN; gs->min_mapq = CSP_MIN_MAPQ;
        gs->max_flag = CSP_MAX_FLAG;
    }
}

/* print global settings. */
static void gll_setting_print(FILE *fp, global_settings *gs, char *prefix) {
    if (gs) {
        int i;
        fprintf(fp, "%snum of input files = %d\n", prefix, gs->nin);
        fprintf(fp, "%sout_dir = %s\n", prefix, gs->out_dir);
        fprintf(fp, "%sis_out_zip = %d, is_genotype = %d\n", prefix, gs->is_out_zip, gs->is_genotype);
        fprintf(fp, "%snum_of_pos = %lu\n", prefix, csp_snplist_size(gs->pl));
        fprintf(fp, "%snum_of_barcodes = %d, num_of_samples = %d\n", prefix, gs->nbarcode, gs->nsid);
        fprintf(fp, "%s%d chroms: ", prefix, gs->nchrom);
        for (i = 0; i < gs->nchrom; i++) fprintf(fp, "%s ", gs->chrom_all[i]);
        fputc('\n', fp);
        fprintf(fp, "%scell-tag = %s, umi-tag = %s\n", prefix, gs->cell_tag, gs->umi_tag);
        fprintf(fp, "%snum_of_threads = %d\n", prefix, gs->nthread);
        fprintf(fp, "%smin_count = %d, min_maf = %.2f, double_gl = %d\n", prefix, gs->min_count, gs->min_maf, gs->double_gl);
        fprintf(fp, "%smin_len = %d, min_mapq = %d\n", prefix, gs->min_len, gs->min_mapq);
        fprintf(fp, "%smax_flag = %d\n", prefix, gs->max_flag);
    }
}

// SZ_NUMERIC_OP_INIT(csp_s, size_t);

#define CSP_VCF_CELLS_HEADER "##fileformat=VCFv4.2\n" 																							\
    "##source=cellSNP_v" CSP_VERSION "\n"																								\
    "##FILTER=<ID=PASS,Description=\"All filters passed\">\n"																			\
    "##FILTER=<ID=.,Description=\"Filter info not available\">\n"																		\
    "##INFO=<ID=DP,Number=1,Type=Integer,Description=\"total counts for ALT and REF\">\n"             									\
    "##INFO=<ID=AD,Number=1,Type=Integer,Description=\"total counts for ALT\">\n"														\
    "##INFO=<ID=OTH,Number=1,Type=Integer,Description=\"total counts for other bases from REF and ALT\">\n"							\
    "##FORMAT=<ID=GT,Number=1,Type=String,Description=\"Genotype\">\n"																\
    "##FORMAT=<ID=PL,Number=G,Type=Integer,Description=\"List of Phred-scaled genotype likelihoods\">\n"							\
    "##FORMAT=<ID=DP,Number=1,Type=Integer,Description=\"total counts for ALT and REF\">\n"											\
    "##FORMAT=<ID=AD,Number=1,Type=Integer,Description=\"total counts for ALT\">\n"														\
    "##FORMAT=<ID=OTH,Number=1,Type=Integer,Description=\"total counts for other bases from REF and ALT\">\n"						\
    "##FORMAT=<ID=ALL,Number=5,Type=Integer,Description=\"total counts for all bases in order of A,C,G,T,N\">\n"

#define CSP_VCF_CELLS_CONTIG "##contig=<ID=1>\n##contig=<ID=2>\n##contig=<ID=3>\n##contig=<ID=4>\n##contig=<ID=5>\n"                             \
    "##contig=<ID=6>\n##contig=<ID=7>\n##contig=<ID=8>\n##contig=<ID=9>\n##contig=<ID=10>\n"									\
    "##contig=<ID=11>\n##contig=<ID=12>\n##contig=<ID=13>\n##contig=<ID=14>\n##contig=<ID=15>\n"									\
    "##contig=<ID=16>\n##contig=<ID=17>\n##contig=<ID=18>\n##contig=<ID=19>\n##contig=<ID=20>\n"									\
    "##contig=<ID=21>\n##contig=<ID=22>\n##contig=<ID=X>\n##contig=<ID=Y>\n"

#define CSP_MTX_HEADER "%%MatrixMarket matrix coordinate integer general\n"                                                         \
    "%\n"

#define CSP_VCF_BASE_HEADER "##fileformat=VCFv4.2\n"

/*@abstract  Set values for internal variables of csp_mplp_t to prepare for pileup. 
@param mplp  Pointer of csp_mplp_t structure.
@param gs    Pointer of global_settings structure.
@return      0 if success, -1 otherwise.
 */
static int csp_mplp_prepare(csp_mplp_t *mplp, global_settings *gs) {
    char **sgnames;
    int i, nsg;
    csp_plp_t *plp;
    /* init HashMap, pool of ul, pool of uu for mplp. */
    mplp->hsg = csp_map_sg_init();
    if (NULL == mplp->hsg) { fprintf(stderr, "[E::%s] could not init csp_map_sg_t structure.\n", __func__); return -1; }
    if (use_umi(gs)) {
        #if DEVELOP
            mplp->pl = csp_pool_ul_init();
            if (NULL == mplp->pl) { fprintf(stderr, "[E::%s] could not init csp_pool_ul_t structure.\n", __func__); return -1; }
            mplp->pu = csp_pool_uu_init();
            if (NULL == mplp->pu) { fprintf(stderr, "[E::%s] could not init csp_pool_uu_t structure.\n", __func__); return -1; }
        #endif
        mplp->su = csp_pool_ps_init();
        if (NULL == mplp->su) { fprintf(stderr, "[E::%s] could not init csp_pool_su_t structure.\n", __func__); return -1; }
    }
    /* set sample names for sample groups. */
    if (use_barcodes(gs)) { sgnames = gs->barcodes; nsg = gs->nbarcode; }
    else if (use_sid(gs)) { sgnames = gs->sample_ids; nsg = gs->nsid; }
    else { fprintf(stderr, "[E::%s] failed to set sample names.\n", __func__); return -1; }  // should not come here!
    if (csp_mplp_set_sg(mplp, sgnames, nsg) < 0) { fprintf(stderr, "[E::%s] failed to set sample names.\n", __func__); return -1; }
    /* init plp for each sample group in mplp->hsg and init HashMap plp->hug for UMI grouping. */
    for (i = 0; i < nsg; i++) {
        if (NULL == (plp = csp_map_sg_val(mplp->hsg, mplp->hsg_iter[i]))) { 
            if (NULL == (csp_map_sg_val(mplp->hsg, mplp->hsg_iter[i]) = plp = csp_plp_init())) {
                fprintf(stderr, "[E::%s] failed to init csp_plp_t for sg HashMap of csp_mplp_t.\n", __func__);
                return -1;
            }
        }
        if (use_umi(gs)) {
            plp->hug = csp_map_ug_init();
            if (NULL == plp->hug) { fprintf(stderr, "[E::%s] could not init csp_map_ug_t structure.\n", __func__); return -1; }
        }
    }
    return 0;
}

/*@abstract    Push content of one csp_pileup_t structure into the csp_mplp_t structure.
@param pileup  Pointer of csp_pileup_t structure to be pushed.
@param mplp    Pointer of csp_mplp_t structure pushing into.
@param sid     Index of Sample ID in the input Sample IDs.
@param gs      Pointer of global_settings structure.
@return        0 if success;
               Negative numbers for error:
                 -1, neither barcodes or Sample IDs are used.
                 -2, khash_put error.
               Positive numbers for warning:
                 1, cell-barcode is not in input barcode-list;

@note   1. To speed up, the caller should guarantee that:
           a) the parameters are valid, i.e. mplp and gs must not be NULL. In fact, this function is supposed to be 
              called after csp_mplp_t is created and set names of sample-groups, so mplp, mplp->hsg could not be NULL.
           b) the csp_pileup_t must have passed the read filtering, refer to pileup_read_with_fetch() for details.
           c) each key (sample group name) in csp_map_sg_t already has a valid, not NULL, value (csp_plp_t*);
              This usually can be done by calling csp_mplp_prepare().
        2. This function is expected to be used by Mode1 & Mode2 & Mode3.

@discuss  In current version, only the result (base and qual) of the first read in one UMI group will be used for mplp statistics.
          TODO: store results of all reads in one UMI group (maybe could do consistency correction in each UMI group) and then 
          do mplp statistics.
 */
static int csp_mplp_push(csp_pileup_t *pileup, csp_mplp_t *mplp, int sid, global_settings *gs) {
    csp_map_sg_iter k;
    csp_map_ug_iter u;
    csp_plp_t *plp = NULL;
    char **s;
    int r, idx;
    /* Push one csp_pileup_t into csp_mplp_t.
    *  The pileup->cb, pileup->umi could not be NULL as the pileuped read has passed filtering.
    */
    if (use_barcodes(gs)) { 
        if ((k = csp_map_sg_get(mplp->hsg, pileup->cb)) == csp_map_sg_end(mplp->hsg)) { return 1; }
        plp = csp_map_sg_val(mplp->hsg, k);
    } else if (use_sid(gs)) { 
        plp = csp_map_sg_val(mplp->hsg, mplp->hsg_iter[sid]);
    } else { return -1; }  // should not come here!
    if (use_umi(gs)) {
        u = csp_map_ug_get(plp->hug, pileup->umi);
        if (u == csp_map_ug_end(plp->hug)) {
            s = csp_pool_ps_get(mplp->su);
            *s = strdup(pileup->umi);
            u = csp_map_ug_put(plp->hug, *s, &r);
            if (r < 0) { return -2; }
            /* An example for pushing base & qual into HashMap of umi group.
            csp_list_uu_t *ul = csp_pool_ul_get(mplp->pl);
            csp_umi_unit_t *uu = csp_pool_uu_get(mplp->pu);
            uu->base = pileup->base; uu->qual = pileup->qual;
            csp_list_uu_push(ul, uu);
            csp_map_ug_val(plp->hug, u) = ul;
             */
            idx = seq_nt16_idx2int(pileup->base);
            plp->bc[idx]++;
            csp_list_qu_push(plp->qu[idx], pileup->qual);
        } // else: do nothing.
    } else {
        idx = seq_nt16_idx2int(pileup->base);
        plp->bc[idx]++;
        csp_list_qu_push(plp->qu[idx], pileup->qual);
    }
    return 0;
}

/*@abstract    Do statistics and filtering after all pileup results have been pushed.
@param mplp    Pointer of csp_mplp_t structure.
@param gs      Pointer of global_settings structure.
@return        0 if success; -1 if error; 1 if not passing filters.

@discuss  In current version, only the result (base and qual) of the first read in one UMI group will be used for mplp statistics.
          TODO: store results of all reads in one UMI group (maybe could do consistency correction in each UMI group) and then 
          do mplp statistics.
 */
static int csp_mplp_stat(csp_mplp_t *mplp, global_settings *gs) {
    csp_plp_t *plp = NULL;
    int i, j, k;
    size_t l;
    for (i = 0; i < mplp->nsg; i++) {
        plp = csp_map_sg_val(mplp->hsg, mplp->hsg_iter[i]);
        for (j = 0; j < 5; j++) { 
            plp->tc += plp->bc[j]; 
            mplp->bc[j] += plp->bc[j];
        }
    }
    for (i = 0; i < 5; i++) { mplp->tc += mplp->bc[i]; }
    if (mplp->tc < gs->min_count) { return 1; }
    csp_infer_allele(mplp->bc, &mplp->inf_rid, &mplp->inf_aid);   // must be called after mplp->bc are completely calculated.
    if (mplp->bc[mplp->inf_aid] < mplp->tc * gs->min_maf) { return 1; }
    if (mplp->ref_idx < 0 || mplp->alt_idx < 0) {  // ref or alt is not valid. Refer to csp_mplp_t.
        mplp->ref_idx = mplp->inf_rid;
        mplp->alt_idx = mplp->inf_aid;
    }
    mplp->ad = mplp->bc[mplp->alt_idx]; mplp->dp = mplp->bc[mplp->ref_idx] + mplp->ad; mplp->oth = mplp->tc - mplp->dp;
    for (i = 0; i < mplp->nsg; i++) {
        plp = csp_map_sg_val(mplp->hsg, mplp->hsg_iter[i]);
        plp->ad = plp->bc[mplp->alt_idx]; if (plp->ad) mplp->nr_ad++;
        plp->dp = plp->bc[mplp->ref_idx] + plp->ad; if (plp->dp) mplp->nr_dp++;
        plp->oth = plp->tc - plp->dp; if (plp->oth) mplp->nr_oth++;
        if (gs->is_genotype) {
            for (j = 0; j < 5; j++) {
                for (l = 0; l < csp_list_qu_size(plp->qu[j]); l++) {
                    if (get_qual_vector(csp_list_qu_A(plp->qu[j], l), 45, 0.25, mplp->qvec) < 0) { return -1; }
                    for (k = 0; k < 4; k++) plp->qmat[j][k] += mplp->qvec[k];
                }
            }
            if (qual_matrix_to_geno(plp->qmat, plp->bc, mplp->ref_idx, mplp->alt_idx, gs->double_gl, plp->gl, &plp->ngl) < 0) { return -1; }
        }
    }
    return 0;
}

/*@abstract  Pileup one read obtained by sam_itr_next().
@param pos   Pos of the reference sequence. 0-based.
@param p     Pointer of csp_pileup_t structure coming from csp_pileup_init() or csp_pileup_reset().
@param gs    Pointer of global settings.
@return      0 if success, -1 if error, 1 if the reads extracted are not in proper format, 2 if not passing filters.

@note        1. This function is modified from cigar_resolve2() function in sam.c of htslib.
             2. Reads filtering is also applied inside this function, including:
                   UMI and cell tags, read mapping quality, mapping flag and length of bases within alignment.
             3. To speed up, parameters will not be checked, so the caller should guarantee the parameters are valid, i.e.
                && p != NULL && gs != NULL.

@TODO        Filter unmapped reads (the read itself unmapped or the mate read unmapped) ?
 */
static int pileup_read_with_fetch(hts_pos_t pos, csp_pileup_t *p, global_settings *gs) {
    /* Filter reads in order. For example, filtering according to umi tag and cell tag would speed up in the case
       that do not use UMI or Cell-barcode at all. */
    if (use_umi(gs) && NULL == (p->umi = get_bam_aux_str(p->b, gs->umi_tag))) { return 1; }
    if (use_barcodes(gs) && NULL == (p->cb = get_bam_aux_str(p->b, gs->cell_tag))) { return 1; }
    bam1_core_t *c = &(p->b->core);
    if (c->qual < gs->min_mapq) { return 2; }
    if (c->flag > gs->max_flag) { return 2; }
    uint32_t *cigar = bam_get_cigar(p->b);
    hts_pos_t x, px;       /* x is the coordinate of the reference. */
    int k, y, py, op, l;   /* y is the query coordinate. */
    uint32_t laln;
    assert(c->pos <= pos);   // otherwise a bug.
    /* find the pos. */
    p->qpos = 0; p->is_refskip = p->is_del = 0;
    for (k = 0, px = x = c->pos, py = y = 0, laln = 0; k < c->n_cigar; k++, px = x, py = y) {
        op = get_cigar_op(cigar[k]);
        l = get_cigar_len(cigar[k]);
        if (op == BAM_CMATCH || op == BAM_CEQUAL || op == BAM_CDIFF) { x += l; y += l; laln += l; }
        else if (op == BAM_CDEL || op == BAM_CREF_SKIP) { x += l; }
        else if (op == BAM_CINS || op == BAM_CSOFT_CLIP) { y += l; }
        // else, do nothing.
        if (x > pos) { break; }
    }
    /* pileup */
    assert(k < c->n_cigar);   // otherwise a bug.
    if (op == BAM_CMATCH || op == BAM_CEQUAL || op == BAM_CDIFF) {
        p->qpos = py + (pos - px); 
        p->base = bam_seqi(bam_get_seq(p->b), p->qpos);
        p->qual = bam_get_qual(p->b)[p->qpos];
    } else if (op == BAM_CDEL || op == BAM_CREF_SKIP) {
        p->is_del = 1; p->qpos = py; // FIXME: distinguish D and N!!!!!
        p->is_refskip = (op == BAM_CREF_SKIP);
    } // cannot be other operations; otherwise a bug
    if (p->is_del) { return 2; }
    if (p->is_refskip) { return 2; }
    /* continue processing cigar string. */
    for (k++; k < c->n_cigar; k++) {
        op = get_cigar_op(cigar[k]);
        l = get_cigar_len(cigar[k]);
        if (op == BAM_CMATCH || op == BAM_CEQUAL || op == BAM_CDIFF) { laln += l; }
    }
    if (laln < gs->min_len) { return 2; }
    else { p->laln = laln; }
    return 0;
}

/*@abstract    Pileup one SNP with method fetch.
@param snp     Pointer of csp_snp_t structure.
@param fs      Pointer of array of pointers to the csp_bam_fs structures.
@param nfs     Size of @p fs.
@param pileup  Pointer of csp_pileup_t structure.
@param mplp    Pointer of csp_mplp_t structure.
@param gs      Pointer of global_settings structure.
@return        0 if success, -1 if error, 1 if pileup failure without error.

@note          1. This function is mainly called by pileup_positions_with_fetch(). Refer to pileup_positions_with_fetch() for notes.
               2. The statistics result of all pileuped reads for one SNP is stored in the csp_mplp_t after calling this function.
*/
static int pileup_snp_with_fetch(csp_snp_t *snp, csp_bam_fs **fs, int nfs, csp_pileup_t *pileup, csp_mplp_t *mplp, global_settings *gs) 
{
    csp_bam_fs *bs = NULL;
    hts_itr_t *iter = NULL;
    int i, tid, r, ret, st, state = -1;
    size_t npushed = 0;
    kstring_t ks = KS_INITIALIZE, *s = &ks;
    #if DEBUG
        size_t npileup = 0;
    #endif
    mplp->ref_idx = snp->ref ? seq_nt16_char2int(snp->ref) : -1;
    mplp->alt_idx = snp->alt ? seq_nt16_char2int(snp->alt) : -1;
    for (i = 0; i < nfs; i++) {
        bs = fs[i];
        tid = csp_sam_hdr_name2id(bs->hdr, snp->chr, s);
        ks_clear(s);
        if (tid < 0) { state = 1; goto fail; }
        if (NULL == (iter = sam_itr_queryi(bs->idx, tid, snp->pos, snp->pos + 1))) { state = 1; goto fail; }
        while ((ret = sam_itr_next(bs->fp, iter, pileup->b)) >= 0) {   // TODO: check if need to be reset in_fp?
            #if DEBUG
                npileup++;
            #endif
            if (0 == (st = pileup_read_with_fetch(snp->pos, pileup, gs))) { // no need to reset pileup as the values in it will be immediately overwritten.
                if (use_barcodes(gs)) { r = csp_mplp_push(pileup, mplp, -1, gs); }
                else if (use_sid(gs)) { r = csp_mplp_push(pileup, mplp, i, gs); }
                else { state = -1; goto fail; }
                if (r < 0) { state = -1; goto fail; }  // else if r == 1: pileuped barcode is not in the input barcode list.
                else if (r == 0) { npushed++; }
            } else if (st < 0) { state = -1; goto fail; }
        }
        if (ret < -1) { state = -1; goto fail; } 
        else { hts_itr_destroy(iter); iter = NULL; }  // TODO: check if could reset iter?
    }
    #if DEBUG
        fprintf(stderr, "[D::%s] before mplp statistics: npileup = %ld; npushed = %ld; the mplp is:\n", __func__, npileup, npushed);
        csp_mplp_print_(stderr, mplp, "\t");
    #endif
    if (npushed < gs->min_count) { state = 1; goto fail; }
    if ((ret = csp_mplp_stat(mplp, gs)) != 0) { state = (ret > 0) ? 1 : -1; goto fail; }
    #if DEBUG
        fprintf(stderr, "[D::%s] after mplp statistics: the mplp is:\n", __func__);
        csp_mplp_print_(stderr, mplp, "\t");
    #endif
    ks_free(s); s = NULL;
    return 0;
  fail:
    if (s) { ks_free(s); }
    if (iter) { hts_itr_destroy(iter); }
    return state;
}

/*@abstract  Pileup a region (a list of SNPs) with method of fetching.
@param args  Pointer to thread_data structure.
@return      Num of SNPs, including those filtered, that are processed.

@note        1. Differ from pileup method in samtools, this function fetches reads covering the SNPs and 
                pileups the reads by processing CIGAR strings with a self-defined resolver function.
             2. The internal variable "ret" in thread_data structure saves the running state of the function:
                  0 if success, 
                  -1 otherwise.
             3. This function could be used by Mode1 and Mode3.		 
 */
static size_t pileup_positions_with_fetch(void *args) {
    thread_data *d = (thread_data*) args;
    global_settings *gs = d->gs;
    csp_snp_t **a = gs->pl.a + d->n;  /* here we use directly the internal variables in csp_snplist_t structure to speed up. */
    size_t n = 0;             /* n is the num of SNPs that are successfully processed. */
    csp_bam_fs **bam_fs = NULL;       /* use array instead of single element to compatible with multi-input-files. */
    int nfs = 0;
    csp_bam_fs *bs = NULL;
    csp_pileup_t *pileup = NULL;
    csp_mplp_t *mplp = NULL;
    int i, ret;
    kstring_t ks = KS_INITIALIZE, *s = &ks;
#if DEBUG
    fprintf(stderr, "[D::%s][Thread-%d] thread options:\n", __func__, d->i);
    thdata_print(stderr, d);
#endif
    d->ret = -1;
    d->ns = d->nr_ad = d->nr_dp = d->nr_oth = 0;
    /* prepare data and structures. 
    */
    if (csp_fs_open(d->out_mtx_ad, NULL) <= 0) { 
        fprintf(stderr, "[E::%s] failed to open tmp mtx AD file '%s'.\n", __func__, d->out_mtx_ad->fn);
        goto fail;
    }
    if (csp_fs_open(d->out_mtx_dp, NULL) <= 0) { 
        fprintf(stderr, "[E::%s] failed to open tmp mtx DP file '%s'.\n", __func__, d->out_mtx_dp->fn);
        goto fail;
    }
    if (csp_fs_open(d->out_mtx_oth, NULL) <= 0) { 
        fprintf(stderr, "[E::%s] failed to open tmp mtx OTH file '%s'.\n", __func__, d->out_mtx_oth->fn);
        goto fail;
    }
    if (csp_fs_open(d->out_vcf_base, NULL) <= 0) { 
        fprintf(stderr, "[E::%s] failed to open tmp vcf BASE file '%s'.\n", __func__, d->out_vcf_base->fn);
        goto fail;
    }
    if (gs->is_genotype) {
        if (csp_fs_open(d->out_vcf_cells, NULL) <= 0) { 
            fprintf(stderr, "[E::%s] failed to open tmp vcf CELLS file '%s'.\n", __func__, d->out_vcf_cells->fn);
            goto fail;
        }
    }
    /* prepare mplp for pileup. */
    if (NULL == (mplp = csp_mplp_init())) { fprintf(stderr, "[E::%s] could not init csp_mplp_t structure.\n", __func__); goto fail; }
    if (csp_mplp_prepare(mplp, gs) < 0) { fprintf(stderr, "[E::%s] could not prepare csp_mplp_t structure.\n", __func__); goto fail; }
    /* create file structures for input bam/sam/cram. */
    bam_fs = (csp_bam_fs**) calloc(gs->nin, sizeof(csp_bam_fs*));  	
    if (NULL == bam_fs) { fprintf(stderr, "[E::%s] could not initialize csp_bam_fs array.\n", __func__); goto fail; }
    for (nfs = 0; nfs < gs->nin; nfs++) {
        if (NULL == (bs = csp_bam_fs_build(gs->in_fns[nfs], &ret))) {
            fprintf(stderr, "[E::%s] could not build csp_bam_fs structure.\n", __func__);
            goto fail;
        } else { bam_fs[nfs] = bs; }
    }
    if (NULL == (pileup = csp_pileup_init())) { 
        fprintf(stderr, "[E::%s] Out of memory allocating csp_pileup_t struct.\n", __func__); 
        goto fail; 
    }
    #if VERBOSE
        double pos_m, pos_n, pos_r, nprints = 50;
        pos_n = pos_m = d->m / nprints;
        pos_r = 100.0 / d->m;
    #endif
    /* pileup each SNP. 
    */
    for (; n < d->m; n++) {
        #if VERBOSE
            if (n >= pos_n) {
                fprintf(stderr, "[I::%s][Thread-%d] %.2f%% SNPs processed.\n", __func__, d->i, n * pos_r);
                pos_n += pos_m;
                pos_n = pos_n <= d->m ? pos_n : d->m;
            }
        #endif
        #if DEBUG
            fputc('\n', stderr);
            fprintf(stderr, "[D::%s] chr = %s; pos = %ld; ref = %c; alt = %c;\n", __func__, a[n]->chr, a[n]->pos + 1, a[n]->ref, a[n]->alt);
        #endif
        if ((ret = pileup_snp_with_fetch(a[n], bam_fs, nfs, pileup, mplp, gs)) != 0) {
            if (ret < 0) {
                fprintf(stderr, "[E::%s] failed to pileup snp (%s:%ld)\n", __func__, a[n]->chr, a[n]->pos + 1);
                goto fail; 
            }
            #if DEBUG
                fprintf(stderr, "[W::%s] snp (%s:%ld) filtered, error code = %d\n", __func__, a[n]->chr, a[n]->pos + 1, ret);
            #endif
            csp_mplp_reset(mplp); ks_clear(s);
            continue;
        } else { d->ns++; }
        d->nr_ad += mplp->nr_ad; d->nr_dp += mplp->nr_dp; d->nr_oth += mplp->nr_oth;
        /* output mplp to mtx and vcf. */
        csp_mplp_to_mtx(mplp, d->out_mtx_ad, d->out_mtx_dp, d->out_mtx_oth, d->ns);
        ksprintf(s, "%s\t%ld\t.\t%c\t%c\t.\tPASS\tAD=%ld;DP=%ld;OTH=%ld", a[n]->chr, a[n]->pos + 1, \
                seq_nt16_int2char(mplp->ref_idx), seq_nt16_int2char(mplp->alt_idx), mplp->ad, mplp->dp, mplp->oth);
        csp_fs_puts(ks_str(s), d->out_vcf_base); csp_fs_putc('\n', d->out_vcf_base);
        if (gs->is_genotype) {
            csp_fs_puts(ks_str(s), d->out_vcf_cells);
            csp_fs_puts("\tGT:AD:DP:OTH:PL:ALL", d->out_vcf_cells);
            csp_mplp_to_vcf(mplp, d->out_vcf_cells);
            csp_fs_putc('\n', d->out_vcf_cells);
        }
        csp_mplp_reset(mplp); ks_clear(s);
    }
    ks_free(s); s = NULL;
    csp_fs_close(d->out_mtx_ad); csp_fs_close(d->out_mtx_dp); csp_fs_close(d->out_mtx_oth);
    csp_fs_close(d->out_vcf_base); if (gs->is_genotype) { csp_fs_close(d->out_vcf_cells); }
    csp_pileup_destroy(pileup);
    for (i = 0; i < nfs; i++) csp_bam_fs_destroy(bam_fs[i]);
    free(bam_fs);
    csp_mplp_destroy(mplp);
    d->ret = 0;
    return n;
  fail:
    if (s) { ks_free(s); }
    if (csp_fs_isopen(d->out_mtx_ad)) { csp_fs_close(d->out_mtx_ad); }
    if (csp_fs_isopen(d->out_mtx_dp)) { csp_fs_close(d->out_mtx_dp); }
    if (csp_fs_isopen(d->out_mtx_oth)) { csp_fs_close(d->out_mtx_oth); }
    if (csp_fs_isopen(d->out_vcf_base)) { csp_fs_close(d->out_vcf_base); }
    if (gs->is_genotype && csp_fs_isopen(d->out_vcf_cells)) { csp_fs_close(d->out_vcf_cells); }
    if (pileup) csp_pileup_destroy(pileup);
    if (bam_fs) {
        for (i = 0; i < nfs; i++) csp_bam_fs_destroy(bam_fs[i]);
        free(bam_fs);		
    }
    if (mplp) { csp_mplp_destroy(mplp); }
    return n;
}

/*@abstract    Create csp_fs_t structure for tmp file.
@param fs      The file struct that the tmp file is based on.
@param idx     A number as suffix.
@param is_zip  If the tmp files should be zipped.
@param s       Pointer of kstring_t.
@return        Pointer to csp_fs_t for tmp file if success, NULL otherwise.
 */
static inline csp_fs_t* create_tmp_fs(csp_fs_t *fs, int idx, int is_zip, kstring_t *s) {
    csp_fs_t *t;
    if (NULL == (t = csp_fs_init())) { return NULL; }
    ksprintf(s, "%s.%d", fs->fn, idx); 
    t->fn = strdup(ks_str(s)); t->fm = "wb"; t->is_zip = is_zip; t->is_tmp = 1;
    return t;
}

/*@abstract    Create array of tmp filen structures based on the given file structure.
@param fs      The file struct that the tmp file structs are based on.
@param n       Number of tmp file structs to be created.
@param is_zip  If the tmp files should be zipped.
@return        Pointer to the array of tmp file structs if success, NULL otherwise. 
 */
static csp_fs_t** create_tmp_files(csp_fs_t *fs, int n, int is_zip) {
    kstring_t ks = KS_INITIALIZE, *s = &ks;
    csp_fs_t *t = NULL, **tfs = NULL;
    int i, j;
    if (NULL == (tfs = (csp_fs_t**) calloc(n, sizeof(csp_fs_t*)))) { goto fail; }
    for (i = 0; i < n; i++) {
        if (NULL == (t = create_tmp_fs(fs, i, is_zip, s))) { goto fail; }
        else { tfs[i] = t; ks_clear(s); }
    } ks_free(s);
    return tfs;
  fail:
    ks_free(s);
    if (tfs) {
        for (j = 0; j < i; j++) csp_fs_destroy(tfs[j]);
        free(tfs);
    }
    return NULL; 
}

/*@abstract  Remove tmp files and free memory.
@param fs    Pointer of array of csp_fs_t structures to be removed and freed.
@param n     Size of array.
@return      Num of tmp files that are removed if no error, -1 otherwise.
 */
static inline int destroy_tmp_files(csp_fs_t **fs, const int n) {
    int i, m;
    m = csp_fs_remove_all(fs, n);
    for (i = 0; i < n; i++) { csp_fs_destroy(fs[i]); }
    free(fs);
    return m;
}

/*@abstract   Merge several tmp sparse matrices files.
@param out    Pointer of file structure merged into.
@param in     Pointer of array of tmp mtx files to be merged.
@param n      Num of tmp mtx files.
@param ns     Pointer to num of SNPs in all input mtx files.
@param nr     Pointer to num of records in all input mtx files.
@param ret    Pointer to the running state. 0 if success, negative numbers for error:
                -1, unknown error;
                -2, I/O error.
@return       Num of tmp mtx files that are successfully merged.
*/
static int merge_mtx(csp_fs_t *out, csp_fs_t **in, const int n, size_t *ns, size_t *nr, int *ret) {
    size_t k = 1, m = 0;
    int i = 0;
    kstring_t in_ks = KS_INITIALIZE, *in_buf = &in_ks;
    *ret = -1;
    if (! csp_fs_isopen(out) && csp_fs_open(out, NULL) <= 0) { *ret = -2; goto fail; }
    for (; i < n; i++) {
        if (csp_fs_open(in[i], "rb") <= 0) { *ret = -2; goto fail; }
        while (csp_fs_getln(in[i], in_buf) >= 0) {
            if (0 == ks_len(in_buf)) {    // empty line, meaning ending of a SNP.
                k++;
            } else {
                csp_fs_printf(out, "%ld\t%s\n", k, ks_str(in_buf));
                m++; ks_clear(in_buf);
            }
        }
        csp_fs_close(in[i]);
    }
    ks_free(in_buf); in_buf = NULL;
    *ns = k - 1; *nr = m;
    *ret = 0; 
    return i;
  fail:
    if (in_buf) { ks_free(in_buf); }
    if (i < n && csp_fs_isopen(in[i])) { csp_fs_close(in[i]); }
    return i;
}

/*@abstract   Merge several tmp vcf files.
@param out    Pointer of file structure merged into.
@param in     Pointer of array of tmp vcf files to be merged.
@param n      Num of tmp vcf files.
@param ret    Pointer to the running state. 0 if success, negative numbers for error:
                -1, unknown error;
                -2, I/O error.
@return       Num of tmp vcf files that are successfully merged.
*/
static int merge_vcf(csp_fs_t *out, csp_fs_t **in, const int n, int *ret) {
#define TMP_BUFSIZE 1048576
    size_t lr, lw;
    char buf[TMP_BUFSIZE];
    int i = 0;
    *ret = -1;
    if (! csp_fs_isopen(out) && csp_fs_open(out, NULL) <= 0) { *ret = -2; goto fail; }
    for (; i < n; i++) {
        if (csp_fs_open(in[i], "rb") <= 0) { *ret = -2; goto fail; }
        while ((lr = csp_fs_read(in[i], buf, TMP_BUFSIZE)) > 0) {
            lw = csp_fs_write(out, buf, lr);
            if (lw != lr) { *ret = -2; goto fail; }
        }
        csp_fs_close(in[i]);
    }
    *ret = 0;
    return i;
  fail:
    if (i < n && csp_fs_isopen(in[i])) { csp_fs_close(in[i]); }
    return i;
#undef TMP_BUFSIZE
}

/*@abstract  Rewrite mtx file to fill in the stat info.
@param fs    Pointer of csp_fs_t that to be rewriten.
@param ns    Num of SNPs.
@param nsmp  Num of samples.
@param nr    Num of records.
@return      0 if success, -1 if error, 1 if the original file has no records while nr != 0.

@note        1. When proc = 1, the origial outputed mtx file was not filled with stat info:
                (totol SNPs, total samples, total records),
                so use this function to fill and rewrite.
             2. @p fs is not open when this function is just called and will keep not open when this function ends.
 */
static int rewrite_mtx(csp_fs_t *fs, size_t ns, int nsmp, size_t nr) {
#define TMP_BUFSIZE 1048576
    kstring_t ks = KS_INITIALIZE, *s = &ks;
    csp_fs_t *new = NULL;
    char buf[TMP_BUFSIZE];
    int r, ret = -1;
    size_t lr, lw;
    if (NULL == (new = create_tmp_fs(fs, 0, fs->is_zip, s))) { goto fail; }
    ks_clear(s);
    if (csp_fs_open(fs, "rb") <= 0 || csp_fs_open(new, "wb") <= 0) { goto fail; }
    while ((r = csp_fs_getln(fs, s)) >= 0 && ks_len(s) && ks_str(s)[0] == '%') {
        csp_fs_puts(ks_str(s), new); csp_fs_putc('\n', new);
        ks_clear(s);
    }
    if (r < 0 || 0 == ks_len(s)) { // has no records. TODO: distinguish EOF and error when r = 1.
        if (nr != 0) { ret = 1; goto fail; }
    }
    csp_fs_printf(new, "%ld\t%d\t%ld\n", ns, nsmp, nr);
    if (nr) { 
        csp_fs_puts(ks_str(s), new); csp_fs_putc('\n', new);
        ks_clear(s);
    }  
    while ((lr = csp_fs_read(fs, buf, TMP_BUFSIZE)) > 0) {
        lw = csp_fs_write(new, buf, lr);
        if (lw != lr) { goto fail; }
    }
    csp_fs_close(fs); csp_fs_close(new);
    csp_fs_remove(fs);
    if (rename(new->fn, fs->fn) != 0) { goto fail; }
    csp_fs_destroy(new); new = NULL;
    ks_free(s);
    return 0;
  fail:
    ks_free(s);
    if (csp_fs_isopen(fs)) { csp_fs_close(fs); }
    if (new) {
        if (csp_fs_isopen(new)) { csp_fs_close(new); }
    }
    return ret;
#undef TMP_BUFSIZE
}

/*abstract  Run cellSNP Mode with method of fetching.
@param gs   Pointer to the global_settings structure.
@return     0 if success, -1 otherwise.
 */
static int run_mode_with_fetch(global_settings *gs) {
    /* check options (input) */
    if (NULL == gs || gs->nin <= 0 || (gs->nbarcode <= 0 && gs->nsid <= 0) || csp_snplist_size(gs->pl) <= 0 || NULL == gs->out_dir) {
        fprintf(stderr, "[E::%s] error options for fetch modes.\n", __func__);
        return -1;
    }
    int nsample = use_barcodes(gs) ? gs->nbarcode : gs->nsid;
    /* core part. */
    if (gs->tp && gs->nthread > 1) {
        int nthread = gs->nthread;
        csp_fs_t **out_tmp_mtx_ad, **out_tmp_mtx_dp, **out_tmp_mtx_oth, **out_tmp_vcf_base, **out_tmp_vcf_cells;
        thread_data **td = NULL, *d = NULL;
        int ntd = 0, mtd = nthread; // ntd: num of thread-data structures that have been created. mtd: size of td array.
        int i, ret;
        size_t npos, mpos, ns, nr_ad, nr_dp, nr_oth, ns_merge, nr_merge;
        out_tmp_mtx_ad = out_tmp_mtx_dp = out_tmp_mtx_oth = out_tmp_vcf_base = out_tmp_vcf_cells = NULL;
        /* create output tmp filenames. */
        if (NULL == (out_tmp_mtx_ad = create_tmp_files(gs->out_mtx_ad, nthread, 0))) {
            fprintf(stderr, "[E::%s] fail to create tmp files for mtx_AD.\n", __func__);
            goto fail;
        }
        if (NULL == (out_tmp_mtx_dp = create_tmp_files(gs->out_mtx_dp, nthread, 0))) {
            fprintf(stderr, "[E::%s] fail to create tmp files for mtx_DP.\n", __func__);
            goto fail;
        }
        if (NULL == (out_tmp_mtx_oth = create_tmp_files(gs->out_mtx_oth, nthread, 0))) {
            fprintf(stderr, "[E::%s] fail to create tmp files for mtx_OTH.\n", __func__);
            goto fail;
        }
        if (NULL == (out_tmp_vcf_base = create_tmp_files(gs->out_vcf_base, nthread, 0))) {
            fprintf(stderr, "[E::%s] fail to create tmp files for vcf_BASE.\n", __func__);
            goto fail;
        }
        if (gs->is_genotype && NULL == (out_tmp_vcf_cells = create_tmp_files(gs->out_vcf_cells, nthread, 0))) {
            fprintf(stderr, "[E::%s] fail to create tmp files for vcf_CELLS.\n", __func__);
            goto fail;
        }
        /* prepare data for thread pool and run. */
        td = (thread_data**) calloc(mtd, sizeof(thread_data*));
        if (NULL == td) { fprintf(stderr, "[E::%s] could not initialize the array of thread_data structure.\n", __func__); goto fail; }
        for (npos = 0, mpos = csp_snplist_size(gs->pl) / mtd; ntd < mtd - 1; ntd++, npos += mpos) { /* mtd is equal to mout_tmp */
            if (NULL == (d = thdata_init())) {
                fprintf(stderr, "[E::%s] could not initialize the thread_data structure.\n", __func__); 
                goto fail; 
            }
            d->gs = gs; d->n = npos; d->m = mpos; d->i = ntd; 
            d->out_mtx_ad = out_tmp_mtx_ad[ntd]; d->out_mtx_dp = out_tmp_mtx_dp[ntd]; d->out_mtx_oth = out_tmp_mtx_oth[ntd];
            d->out_vcf_base = out_tmp_vcf_base[ntd]; d->out_vcf_cells = gs->is_genotype ? out_tmp_vcf_cells[ntd] : NULL;
            td[ntd] = d;
            if (thpool_add_work(gs->tp, (void*) pileup_positions_with_fetch, d) < 0) {
                fprintf(stderr, "[E::%s] could not add thread work (No. %d)\n", __func__, ntd++);
                goto fail;
            } // make sure do not to free pointer d when fail.
        }
        if (csp_snplist_size(gs->pl) - npos > 0) { // still have some SNPs to be processed.
            if (NULL == (d = thdata_init())) { 
                fprintf(stderr, "[E::%s] could not initialize the thread_data structure.\n", __func__); 
                goto fail; 
            }
            d->gs = gs; d->n = npos; d->m = csp_snplist_size(gs->pl) - npos; d->i = ntd; 
            d->out_mtx_ad = out_tmp_mtx_ad[ntd]; d->out_mtx_dp = out_tmp_mtx_dp[ntd]; d->out_mtx_oth = out_tmp_mtx_oth[ntd];
            d->out_vcf_base = out_tmp_vcf_base[ntd]; d->out_vcf_cells = gs->is_genotype ? out_tmp_vcf_cells[ntd] : NULL;
            td[ntd] = d;         
            if (thpool_add_work(gs->tp, (void*) pileup_positions_with_fetch, d) < 0) {
                fprintf(stderr, "[E::%s] could not add thread work (No. %d)\n", __func__, ntd++);
                goto fail;
            } else { ntd++; }
        }
        thpool_wait(gs->tp);
        /* check running status of threads. */
        #if DEBUG
            for (i = 0; i < ntd; i++) { fprintf(stderr, "[D::%s] ret of thread-%d is %d\n", __func__, i, td[i]->ret); }
        #endif
        for (i = 0; i < nthread; i++) { if (td[i]->ret < 0) goto fail; }
        /* merge tmp files. */
        ns = nr_ad = nr_dp = nr_oth = 0;
        for (i = 0; i < nthread; i++) {
            nr_ad += td[i]->nr_ad; nr_dp += td[i]->nr_dp; nr_oth += td[i]->nr_oth;
            ns += td[i]->ns;
        }
        if (csp_fs_open(gs->out_mtx_ad, NULL) < 0) { fprintf(stderr, "[E::%s] failed to open mtx AD.\n", __func__); goto fail; }
        csp_fs_printf(gs->out_mtx_ad, "%ld\t%d\t%ld\n", ns, nsample, nr_ad);
        merge_mtx(gs->out_mtx_ad, out_tmp_mtx_ad, gs->nthread, &ns_merge, &nr_merge, &ret);
        if (ret < 0 || ns_merge != ns || nr_merge != nr_ad) { fprintf(stderr, "[E::%s] failed to merge mtx AD.\n", __func__); goto fail; }
        csp_fs_close(gs->out_mtx_ad);

        if (csp_fs_open(gs->out_mtx_dp, NULL) < 0) { fprintf(stderr, "[E::%s] failed to open mtx DP.\n", __func__); goto fail; }
        csp_fs_printf(gs->out_mtx_dp, "%ld\t%d\t%ld\n", ns, nsample, nr_dp);
        merge_mtx(gs->out_mtx_dp, out_tmp_mtx_dp, gs->nthread, &ns_merge, &nr_merge, &ret);
        if (ret < 0 || ns_merge != ns || nr_merge != nr_dp) { fprintf(stderr, "[E::%s] failed to merge mtx DP.\n", __func__); goto fail; }
        csp_fs_close(gs->out_mtx_dp);

        if (csp_fs_open(gs->out_mtx_oth, NULL) < 0) { fprintf(stderr, "[E::%s] failed to open mtx OTH.\n", __func__); goto fail; }
        csp_fs_printf(gs->out_mtx_oth, "%ld\t%d\t%ld\n", ns, nsample, nr_oth);
        merge_mtx(gs->out_mtx_oth, out_tmp_mtx_oth, gs->nthread, &ns_merge, &nr_merge, &ret);
        if (ret < 0 || ns_merge != ns || nr_merge != nr_oth) { fprintf(stderr, "[E::%s] failed to merge mtx OTH.\n", __func__); goto fail; }
        csp_fs_close(gs->out_mtx_oth);

        if (csp_fs_open(gs->out_vcf_base, NULL) < 0) { fprintf(stderr, "[E::%s] failed to open vcf BASE.\n", __func__); goto fail; }
        merge_vcf(gs->out_vcf_base, out_tmp_vcf_base, gs->nthread, &ret);
        if (ret < 0) { fprintf(stderr, "[E::%s] failed to merge vcf BASE.\n", __func__); goto fail; }
        csp_fs_close(gs->out_vcf_base);

        if (gs->is_genotype) {
            if (csp_fs_open(gs->out_vcf_cells, NULL) < 0) { fprintf(stderr, "[E::%s] failed to open vcf CELLS.\n", __func__); goto fail; }
            merge_vcf(gs->out_vcf_cells, out_tmp_vcf_cells, gs->nthread, &ret);
            if (ret < 0) { fprintf(stderr, "[E::%s] failed to merge vcf CELLS.\n", __func__); goto fail; }    
            csp_fs_close(gs->out_vcf_cells);     
        }
        /* clean */
        for (i = 0; i < ntd; i++) { thdata_destroy(td[i]); }
        free(td); td = NULL;
        if (destroy_tmp_files(out_tmp_mtx_ad, nthread) < 0) {
            fprintf(stderr, "[W::%s] failed to remove tmp mtx AD files.\n", __func__);
        } out_tmp_mtx_ad = NULL;
        if (destroy_tmp_files(out_tmp_mtx_dp, nthread) < 0) {
            fprintf(stderr, "[W::%s] failed to remove tmp mtx DP files.\n", __func__);
        } out_tmp_mtx_dp = NULL;
        if (destroy_tmp_files(out_tmp_mtx_oth, nthread) < 0) {
            fprintf(stderr, "[W::%s] failed to remove tmp mtx OTH files.\n", __func__);
        } out_tmp_mtx_oth = NULL;
        if (destroy_tmp_files(out_tmp_vcf_base, nthread) < 0) {
            fprintf(stderr, "[W::%s] failed to remove tmp vcf BASE files.\n", __func__);
        } out_tmp_vcf_base = NULL;
        if (gs->is_genotype) {         
            if (destroy_tmp_files(out_tmp_vcf_cells, nthread) < 0) {
                fprintf(stderr, "[W::%s] failed to remove tmp vcf CELLS files.\n", __func__);
            } out_tmp_vcf_cells = NULL;
        }
        return 0;
      fail:
        if (td) {
            for (i = 0; i < ntd; i++) { thdata_destroy(td[i]); }
            free(td);
        }
        if (out_tmp_mtx_ad && destroy_tmp_files(out_tmp_mtx_ad, nthread) < 0) {
            fprintf(stderr, "[W::%s] failed to remove tmp mtx AD files.\n", __func__);
        }
        if (out_tmp_mtx_dp && destroy_tmp_files(out_tmp_mtx_dp, nthread) < 0) {
            fprintf(stderr, "[W::%s] failed to remove tmp mtx DP files.\n", __func__);
        }
        if (out_tmp_mtx_oth && destroy_tmp_files(out_tmp_mtx_oth, nthread) < 0) {
            fprintf(stderr, "[W::%s] failed to remove tmp mtx OTH files.\n", __func__);
        }
        if (out_tmp_vcf_base && destroy_tmp_files(out_tmp_vcf_base, nthread) < 0) {
            fprintf(stderr, "[W::%s] failed to remove tmp vcf BASE files.\n", __func__);
        }
        if (out_tmp_vcf_cells && destroy_tmp_files(out_tmp_vcf_cells, nthread) < 0) {
            fprintf(stderr, "[W::%s] failed to remove tmp vcf CELLS files.\n", __func__);
        }
        if (csp_fs_isopen(gs->out_mtx_ad)) { csp_fs_close(gs->out_mtx_ad); }
        if (csp_fs_isopen(gs->out_mtx_dp)) { csp_fs_close(gs->out_mtx_dp); }
        if (csp_fs_isopen(gs->out_mtx_oth)) { csp_fs_close(gs->out_mtx_oth); }
        if (csp_fs_isopen(gs->out_vcf_base)) { csp_fs_close(gs->out_vcf_base); }
        if (gs->is_genotype && csp_fs_isopen(gs->out_vcf_cells)) { csp_fs_close(gs->out_vcf_cells); }
        return -1;
    } else if (1 == gs->nthread) {  // only one thread.
        thread_data *d = NULL;
        //csp_fs_t *out_mtx_ad, *out_mtx_dp, *out_mtx_oth, *out_vcf_base, *out_vcf_cells;
        //kstring_t ks = KS_INITIALIZE, *s = &ks;
        //out_mtx_ad = out_mtx_dp = out_mtx_oth = out_vcf_base = out_vcf_cells = NULL;
        if (NULL == (d = thdata_init())) { 
            fprintf(stderr, "[E::%s] could not initialize the thread_data structure.\n", __func__); 
            goto fail1; 
        }
        /*
        if (NULL == create_tmp_fs(gs->out_mtx_ad, 0, 0, s)) {
            fprintf(stderr, "[E::%s] fail to create tmp mtx_AD.\n", __func__); 
            goto fail1; 
        } else { ks_clear(s); }
        if (NULL == create_tmp_fs(gs->out_mtx_dp, 0, 0, s)) {
            fprintf(stderr, "[E::%s] fail to create tmp mtx_DP.\n", __func__); 
            goto fail1; 
        } else { ks_clear(s); }
        if (NULL == create_tmp_fs(gs->out_mtx_oth, 0, 0, s)) {
            fprintf(stderr, "[E::%s] fail to create tmp mtx_OTH.\n", __func__); 
            goto fail1; 
        } else { ks_clear(s); }
        if (NULL == create_tmp_fs(gs->out_vcf_base, 0, 0, s)) {
            fprintf(stderr, "[E::%s] fail to create tmp vcf BASE.\n", __func__); 
            goto fail1; 
        } else { ks_clear(s); }
        if (gs->is_genotype) {
            if (NULL == create_tmp_fs(gs->out_vcf_cells, 0, 0, s)) {
                fprintf(stderr, "[E::%s] fail to create tmp vcf CELLS.\n", __func__); 
                goto fail1;
            } else { ks_clear(s); }
        }
        d->gs = gs; d->n = 0; d->m = csp_snplist_size(gs->pl); d->i = 0; 
        d->out_mtx_ad = out_mtx_ad; d->out_mtx_dp = out_mtx_dp; d->out_mtx_oth = out_mtx_oth;
        d->out_vcf_base = out_vcf_base; d->out_vcf_cells = gs->is_genotype ? out_vcf_cells : NULL;
        */
        d->gs = gs; d->n = 0; d->m = csp_snplist_size(gs->pl); d->i = 0; 
        d->out_mtx_ad = gs->out_mtx_ad; d->out_mtx_dp = gs->out_mtx_dp; d->out_mtx_oth = gs->out_mtx_oth;
        d->out_vcf_base = gs->out_vcf_base; d->out_vcf_cells = gs->is_genotype ? gs->out_vcf_cells : NULL;
        pileup_positions_with_fetch(d);
        if (d->ret < 0) { goto fail1; }
        if (rewrite_mtx(gs->out_mtx_ad, d->ns, nsample, d->nr_ad) != 0) {
            fprintf(stderr, "[E::%s] failed to rewrite mtx AD.\n", __func__);
            goto fail1;
        }
        if (rewrite_mtx(gs->out_mtx_dp, d->ns, nsample, d->nr_dp) != 0) { 
            fprintf(stderr, "[E::%s] failed to rewrite mtx DP.\n", __func__);
            goto fail1;
        }
        if (rewrite_mtx(gs->out_mtx_oth, d->ns, nsample, d->nr_oth) != 0) { 
            fprintf(stderr, "[E::%s] failed to rewrite mtx OTH.\n", __func__);
            goto fail1;
        }
        thdata_destroy(d); d = NULL;
        return 0;
      fail1:
        if (d) { thdata_destroy(d); }
        return -1;
    } /* else: do nothing. should not come here! */
    return -1;
}

static inline int run_mode1(global_settings *gs) { return run_mode_with_fetch(gs); }

static int run_mode2(global_settings *gs) {
    return 0;
}

static inline int run_mode3(global_settings *gs) { return run_mode_with_fetch(gs); }

static void print_usage(FILE *fp) {
    fprintf(fp, 
"\n"
"Usage: %s [options]\n", CSP_NAME);
    fprintf(fp,
"\n"
"Options:\n"
"  -h, --help           Show this help message and exit.\n"
"  -s, --samFile STR    Indexed sam/bam file(s), comma separated multiple samples.\n"
"                       Mode 1&2: one sam/bam file with single cell.\n"
"                       Mode 3: one or multiple bulk sam/bam files,\n"
"                       no barcodes needed, but sample ids and regionsVCF.\n"
"  -S, --samFileList FILE   A list file containing bam files, each per line, for Mode 3.\n"
"  -O, --outDir DIR         Output directory for VCF and sparse matrices.\n"
"  -R, --regionsVCF FILE    A vcf file listing all candidate SNPs, for fetch each variants.\n" 
"                           If None, pileup the genome. Needed for bulk samples.\n"
"  -b, --barcodeFile FILE   A plain file listing all effective cell barcode.\n"
"  -i, --sampleList FILE    A list file containing sample IDs, each per line.\n"
"  -I, --sampleIDs STR      Comma separated sample ids.\n"
"  --genotype               If use, do genotyping in addition to counting.\n");
    fprintf(fp,
"\n"
"Optional arguments:\n"
"  -p, --nproc INT      Number of subprocesses [%d]\n", CSP_NTHREAD);
    fprintf(fp,
"  --chrom STR          The chromosomes to use, comma separated [1 to %d]\n", CSP_NCHROM);
    fprintf(fp,
"  --cellTAG STR        Tag for cell barcodes, turn off with None [%s]\n", CSP_CELL_TAG);
    fprintf(fp,
"  --UMItag STR         Tag for UMI: UR, Auto, None. For Auto mode, use UR if barcodes is inputted,\n"
"                       otherwise use None. None mode means no UMI but read counts [%s]\n", CSP_UMI_TAG);
    fprintf(fp,
"  --minCOUNT INT       Minimum aggragated count [%d]\n", CSP_MIN_COUNT);
    fprintf(fp,
"  --minMAF FLOAT       Minimum minor allele frequency [%.2f]\n", CSP_MIN_MAF);
    fprintf(fp,
"  --doubletGL          If use, keep doublet GT likelihood, i.e., GT=0.5 and GT=1.5.\n"
"  --gzip               If use, the output files will be zipped.\n"
"\n"
"Read filtering:\n"
"  --minLEN INT         Minimum mapped length for read filtering [%d]\n", CSP_MIN_LEN);
    fprintf(fp,
"  --minMAPQ INT        Minimum MAPQ for read filtering [%d]\n", CSP_MIN_MAPQ);
    fprintf(fp,
"  --maxFLAG INT        Maximum FLAG for read filtering [%d]\n", CSP_MAX_FLAG);
    fputc('\n', fp);
}

static inline int csp_cmp_barcodes(const void *x, const void *y) {
    return strcmp(*((char**) x), *((char**) y));
}

/*@abstract    Perform basic check for global settings right after running getopt()/getopt_long() function.
@param gs      Pointer to the global settings.
@return        0 if no error, negative numbers otherwise:
                 -1, should print_usage after return.
                 -2, no action.

@note          This is just basic check for the shared parameters of different running modes.
               More careful and personalized check would be performed by each running mode.
 */
static int check_global_args(global_settings *gs) {
    int i;
    if (gs->in_fn_file) {
        if (gs->in_fns) { 
            fprintf(stderr, "[E::%s] should not specify -s/--samFile and -S/--samFileList options at the same time.\n", __func__); 
            return -1; 
        } else if (NULL == (gs->in_fns = hts_readlines(gs->in_fn_file, &gs->nin)) || gs->nin <= 0) {
            fprintf(stderr, "[E::%s] could not read '%s'\n", __func__, gs->in_fn_file); 
            return -2;
        }
    } else if (NULL == gs->in_fns) { 
        fprintf(stderr, "[E::%s] should specify -s/--samFile or -S/--samFileList option.\n", __func__); 
        return -1; 
    }
    for (i = 0; i < gs->nin; i++) {
        if (0 != access(gs->in_fns[i], F_OK)) { fprintf(stderr, "[E::%s] '%s' does not exist.\n", __func__, gs->in_fns[i]); return -2; }
    }
    if (gs->out_dir) {
        if (0 != access(gs->out_dir, F_OK) && 0 != mkdir(gs->out_dir, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH)) { 
            fprintf(stderr, "[E::%s] '%s' does not exist.\n", __func__, gs->out_dir); 
            return -2; 
        }
    } else { fprintf(stderr, "[E::%s] should specify -O/--outDir option.\n", __func__); return -1; }
     /* 1. In current version, one and only one of barcodes and sample-ids would exist and work. Prefer barcodes. 
        2. For barcodes, the barcode file would not be read unless cell-tag is set, i.e. the barcodes and cell-tag are
           effective only when both of them are valid. 
        3. Codes below are a little repetitive and redundant, but it works well, maybe improve them in future.
    */
    if (gs->cell_tag && (0 == strcmp(gs->cell_tag, "None") || 0 == strcmp(gs->cell_tag, "none"))) { 
        free(gs->cell_tag);  gs->cell_tag = NULL; 
    }
    if (gs->sample_ids || gs->sid_list_file) {
        if (gs->barcode_file) { fprintf(stderr, "[E::%s] should not specify barcodes and sample IDs at the same time.\n", __func__); return -1; }
        else if (gs->cell_tag) { free(gs->cell_tag); gs->cell_tag = NULL; } 
    }
    if (gs->cell_tag && gs->barcode_file) {
        if (gs->sample_ids || gs->sid_list_file) { 
            fprintf(stderr, "[E::%s] should not specify barcodes and sample IDs at the same time.\n", __func__); 
            return -1; 
        } else if (NULL == (gs->barcodes = hts_readlines(gs->barcode_file, &gs->nbarcode))) {
            fprintf(stderr, "[E::%s] could not read barcode file '%s'\n", __func__, gs->barcode_file); 
            return -2;
        } else { qsort(gs->barcodes, gs->nbarcode, sizeof(char*), csp_cmp_barcodes); }
    } else if ((NULL == gs->cell_tag) ^ (NULL == gs->barcode_file)) {
        fprintf(stderr, "[E::%s] should not specify barcodes or cell-tag alone.\n", __func__); 
        return -1;
    } else {
        if (NULL == gs->sample_ids) {
            if (NULL == gs->sid_list_file) { 
                kstring_t ks = KS_INITIALIZE, *s = &ks;
                for (i = 0; i < gs->nin; i++) { ksprintf(s, "Sample_%d", i); gs->sample_ids[i] = strdup(ks_str(s)); ks_clear(s); }
                ks_free(s);
            } else if (NULL == (gs->sample_ids = hts_readlines(gs->sid_list_file, &gs->nsid))) {
                fprintf(stderr, "[E::%s] could not read '%s'\n", __func__, gs->sid_list_file);  
                return -2;
            } // else: sort sample ids and corresponded input-bam-files?
        } else if (gs->sid_list_file) { 
            fprintf(stderr, "[E::%s] should not specify -i/--samileList and -I/--sampleIDs options at the same time.\n", __func__);
            return -1; 
        } // else do nothing.
        if (gs->nin != gs->nsid) {
            fprintf(stderr, "[E::%s] num of sample IDs (%d) is not equal with num of input bam/sam/cram files (%d).\n", __func__, gs->nsid, gs->nin);
            return -2;
        }
    }
    /* 1. In current version, one and only one of pos_list and chrom(s) would exist and work. Prefer pos_list. 
       2. Sometimes, pos_list_file and chrom_all are both not NULL as the chrom_all has been set to default value when
          global_settings structure was just created. In this case, free chrom_all and save pos_list_file. */
    if (NULL == gs->pos_list_file || 0 == strcmp(gs->pos_list_file, "None") || 0 == strcmp(gs->pos_list_file, "none")) { 
        if (NULL == gs->chrom_all) { fprintf(stderr, "[E::%s] should specify -R/--regionsVCF or --chrom option.\n", __func__); return -1; }
        if (gs->pos_list_file) { free(gs->pos_list_file); gs->pos_list_file = NULL; }
    } else if (gs->chrom_all) { str_arr_destroy(gs->chrom_all, gs->nchrom); gs->chrom_all = NULL; gs->nchrom = 0; }
    if (gs->umi_tag) {
        if (0 == strcmp(gs->umi_tag, "Auto")) {
            if (gs->barcodes) { free(gs->umi_tag); gs->umi_tag = strdup("UR"); }
            else { free(gs->umi_tag); gs->umi_tag = NULL; }
        } else if (0 == strcmp(gs->umi_tag, "None") || 0 == strcmp(gs->umi_tag, "none")) { free(gs->umi_tag); gs->umi_tag = NULL; }
    }
    return 0;
}

/*@abstract    Output headers to files (vcf, mtx etc.)
@param fs      Pointer of csp_fs_t that the header will be writen into.
@param fm      File mode; if NULL, use default file mode inside csp_fs_t.
@param header  Header to be outputed, ends with '\0'.
@param len     Size of header.
@return        0 if success, negative numbers otherwise:
                 -1, open error; -2, write error; -3, close error.
 */
static inline int output_headers(csp_fs_t *fs, char *fm, char *header, size_t len) {
    int ret;
    if (csp_fs_open(fs, fm) <= 0) { return -1; }
    if (csp_fs_puts(header, fs) != len) { ret = -2; goto fail; }
    if (csp_fs_close(fs) < 0) { ret = -3; goto fail; }
    return 0;
  fail:
    if (csp_fs_isopen(fs)) { csp_fs_close(fs); }
    return ret; 
}

static inline char* format_fn(char *fn, int is_zip, kstring_t *s) {
    if (is_zip) {
        kputs(fn, s); kputs(".gz", s);
        return strdup(ks_str(s));
    } else { return fn; }
}

int main(int argc, char **argv) {
    /* timing */
    time_t start_time, end_time;
    struct tm *time_info;
    char time_str[30];
    time(&start_time);
    time_info = localtime(&start_time);
    strftime(time_str, 30, "%Y-%m-%d %H:%M:%S", time_info);
    /* Formal part */
    global_settings gs;
    gll_set_default(&gs);
    kstring_t ks = KS_INITIALIZE, *s = &ks;
    int c, k, ret, print_time = 1;
    struct option lopts[] = {
        {"help", no_argument, NULL, 'h'},
        {"samFile", required_argument, NULL, 's'},
        {"samfile", required_argument, NULL, 's'},
        {"samFileList", required_argument, NULL, 'S'},			
        {"outDir", required_argument, NULL, 'O'},
        {"outdir", required_argument, NULL, 'O'},
        {"regionsVCF", required_argument, NULL, 'R'},
        {"regionsvcf", required_argument, NULL, 'R'},
        {"barcodeFile", required_argument, NULL, 'b'},
        {"barcodefile", required_argument, NULL, 'b'},
        {"sampleList", required_argument, NULL, 'i'},
        {"sampleIDs", required_argument, NULL, 'I'},
        {"sampleids", required_argument, NULL, 'I'},
        {"nproc", required_argument, NULL, 'p'},
        {"chrom", required_argument, NULL, 1},
        {"cellTAG", required_argument, NULL, 2},
        {"celltag", required_argument, NULL, 2},
        {"UMItag", required_argument, NULL, 3},
        {"umitag", required_argument, NULL, 3},
        {"minCOUNT", required_argument, NULL, 4},
        {"minCount", required_argument, NULL, 4},
        {"mincount", required_argument, NULL, 4},
        {"minMAF", required_argument, NULL, 5},
        {"doubleGL", no_argument, NULL, 6},
        {"minLEN", required_argument, NULL, 8},
        {"minLen", required_argument, NULL, 8},
        {"minlen", required_argument, NULL, 8},
        {"minMAPQ", required_argument, NULL, 9},
        {"maxFLAG", required_argument, NULL, 10},
        {"maxFlag", required_argument, NULL, 10},
        {"maxflag", required_argument, NULL, 10},
        {"genotype", no_argument, NULL, 11},
        {"gzip", no_argument, NULL, 12}
    };
    if (1 == argc) { print_usage(stderr); print_time = 0; goto fail; }
    while ((c = getopt_long(argc, argv, "hs:S:O:R:b:i:I:p:", lopts, NULL)) != -1) {
        switch (c) {
            case 'h': print_usage(stderr); print_time = 0; goto fail;
            case 's': 
                    if (gs.in_fns) { str_arr_destroy(gs.in_fns, gs.nin); }
                    if (NULL == (gs.in_fns = hts_readlist(optarg, 0, &gs.nin)) || gs.nin <= 0) {
                        fprintf(stderr, "[E::%s] could not read input-list '%s' or list empty.\n", __func__, optarg);
                        goto fail;
                    } else { break;	}
            case 'S': 
                    if (gs.in_fn_file) free(gs.in_fn_file);
                    gs.in_fn_file = strdup(optarg); break;
            case 'O': 
                    if (gs.out_dir) { free(gs.out_dir); }
                    gs.out_dir = strdup(optarg); break;
            case 'R': 
                    if (gs.pos_list_file) free(gs.pos_list_file);
                    gs.pos_list_file = strdup(optarg); break;
            case 'b': 
                    if (gs.barcode_file) free(gs.barcode_file);
                    gs.barcode_file = strdup(optarg); break;
            case 'i':
                    if (gs.sid_list_file) free(gs.sid_list_file);
                    gs.sid_list_file = strdup(optarg); break;
            case 'I': 
                    if (gs.sample_ids) { str_arr_destroy(gs.sample_ids, gs.nsid); }
                    if (NULL == (gs.sample_ids = hts_readlist(optarg, 0, &gs.nsid))) {
                        fprintf(stderr, "[E::%s] could not read sample-id file '%s'\n", __func__, optarg);
                        goto fail;
                    } else { break; }
            case 'p': gs.nthread = atoi(optarg); break;
            case 1:  
                    if (gs.chrom_all) { str_arr_destroy(gs.chrom_all, gs.nchrom); }
                    if (NULL == (gs.chrom_all = hts_readlist(optarg, 0, &gs.nchrom))) {
                        fprintf(stderr, "[E::%s] could not read chrom-list '%s'\n", __func__, optarg);
                        goto fail;
                    }  else { break; }
            case 2:  
                    if (gs.cell_tag) free(gs.cell_tag);
                    gs.cell_tag = strdup(optarg); break;
            case 3:  
                    if (gs.umi_tag) free(gs.umi_tag);
                    gs.umi_tag = strdup(optarg); break;
            case 4:  gs.min_count = atoi(optarg); break;
            case 5:  gs.min_maf = atof(optarg); break;
            case 6:  gs.double_gl = 1; break;
            case 8:  gs.min_len = atoi(optarg); break;
            case 9:  gs.min_mapq = atoi(optarg); break;
            case 10: gs.max_flag = atoi(optarg); break;
            case 11: gs.is_genotype = 1; break;
            case 12: gs.is_out_zip = 1; break;
            default:  fprintf(stderr,"Invalid option: '%c'\n", c); goto fail;													
        }
    }
    fprintf(stderr, "[I::%s] start time: %s\n", __func__, time_str);
#if DEBUG
    fprintf(stderr, "[D::%s] global settings before checking:\n", __func__);
    gll_setting_print(stderr, &gs, "\t");
#endif
    /* check global settings */
    if ((ret = check_global_args(&gs)) < 0) { 
        fprintf(stderr, "[E::%s] error global settings\n", __func__);
        if (ret == -1) print_usage(stderr);
        goto fail;
    }
#if DEBUG
    fprintf(stderr, "[D::%s] global settings after checking:\n", __func__);
    gll_setting_print(stderr, &gs, "\t");
#endif
    /* prepare running data & options for each thread based on the checked global parameters.*/
    if (gs.nthread > 1 && NULL == (gs.tp = thpool_init(gs.nthread))) {
        fprintf(stderr, "[E::%s] could not initialize the thread pool.\n", __func__);
        goto fail;
    }
    /* prepare output files. */
    if (NULL == (gs.out_mtx_ad = csp_fs_init()) || NULL == (gs.out_mtx_dp = csp_fs_init()) || \
        NULL == (gs.out_mtx_oth = csp_fs_init()) || NULL == (gs.out_samples = csp_fs_init()) || \
        NULL == (gs.out_vcf_base = csp_fs_init()) || (gs.is_genotype && NULL == (gs.out_vcf_cells = csp_fs_init()))) {
        fprintf(stderr, "[E::%s] fail to create csp_fs_t.\n", __func__);
        goto fail;
    }
    gs.out_mtx_ad->is_zip = 0; gs.out_mtx_ad->is_tmp = 0;
    gs.out_mtx_ad->fn = format_fn(join_path(gs.out_dir, CSP_OUT_MTX_AD), gs.out_mtx_ad->is_zip, s); ks_clear(s);
    gs.out_mtx_dp->is_zip = 0; gs.out_mtx_dp->is_tmp = 0;
    gs.out_mtx_dp->fn = format_fn(join_path(gs.out_dir, CSP_OUT_MTX_DP), gs.out_mtx_dp->is_zip, s); ks_clear(s); 
    gs.out_mtx_oth->is_zip = 0; gs.out_mtx_oth->is_tmp = 0;
    gs.out_mtx_oth->fn = format_fn(join_path(gs.out_dir, CSP_OUT_MTX_OTH), gs.out_mtx_oth->is_zip, s); ks_clear(s);
    gs.out_vcf_base->is_zip = gs.is_out_zip; gs.out_vcf_base->is_tmp = 0;
    gs.out_vcf_base->fn = format_fn(join_path(gs.out_dir, CSP_OUT_VCF_BASE), gs.out_vcf_base->is_zip, s); ks_clear(s);
    gs.out_samples->is_zip = 0; gs.out_samples->is_tmp = 0;
    gs.out_samples->fn = format_fn(join_path(gs.out_dir, CSP_OUT_SAMPLES), gs.out_samples->is_zip, s); ks_clear(s);
    if (gs.is_genotype) { 
        gs.out_vcf_cells->is_zip = gs.is_out_zip; gs.out_vcf_cells->is_tmp = 0;
        gs.out_vcf_cells->fn = format_fn(join_path(gs.out_dir, CSP_OUT_VCF_CELLS), gs.out_vcf_cells->is_zip, s); ks_clear(s);
    } // no need to set is_tmp for these out files.
    /* output headers to files. */
    kputs(CSP_MTX_HEADER, s);
    if (output_headers(gs.out_mtx_ad, "wb", ks_str(s), ks_len(s)) < 0) {   // output header to mtx_AD
        fprintf(stderr, "[E::%s] fail to write header to '%s'\n", __func__, gs.out_mtx_ad->fn);
        goto fail;
    }
    if (output_headers(gs.out_mtx_dp, "wb", ks_str(s), ks_len(s)) < 0) {   // output header to mtx_DP
        fprintf(stderr, "[E::%s] fail to write header to '%s'\n", __func__, gs.out_mtx_dp->fn);
        goto fail;
    }
    if (output_headers(gs.out_mtx_oth, "wb", ks_str(s), ks_len(s)) < 0) {  // output header to mtx_OTH
        fprintf(stderr, "[E::%s] fail to write header to '%s'\n", __func__, gs.out_mtx_oth->fn);
        goto fail;
    } ks_clear(s);
    if (use_barcodes(&gs)) {                     // output samples.
        for (k = 0; k < gs.nbarcode; k++) { kputs(gs.barcodes[k], s); kputc('\n', s); }
    } else if (use_sid(&gs)) {
        for (k = 0; k < gs.nsid; k++) { kputs(gs.sample_ids[k], s); kputc('\n', s); }
    }
    if (output_headers(gs.out_samples, "wb", ks_str(s), ks_len(s)) < 0) {
        fprintf(stderr, "[E::%s] fail to write samples to '%s'\n", __func__, gs.out_samples->fn);
        goto fail;
    } ks_clear(s);
    kputs(CSP_VCF_BASE_HEADER, s);             // output header to vcf base.
    kputs("#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\n", s);
    if (output_headers(gs.out_vcf_base, "wb", ks_str(s), ks_len(s)) < 0) {
        fprintf(stderr, "[E::%s] fail to write header to '%s'\n", __func__, gs.out_vcf_base->fn);
        goto fail;
    } ks_clear(s);
    if (gs.is_genotype) {
        kputs(CSP_VCF_CELLS_HEADER, s);           // output header to vcf cells.
        kputs(CSP_VCF_CELLS_CONTIG, s);
        kputs("#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT", s);
        if (use_barcodes(&gs) && gs.barcodes) {
            for (k = 0; k < gs.nbarcode; k++) { kputc_('\t', s); kputs(gs.barcodes[k], s); }
        } else if (use_sid(&gs) && gs.sample_ids) {
            for (k = 0; k < gs.nsid; k++) { kputc_('\t', s); kputs(gs.sample_ids[k], s); }
        } else { fprintf(stderr, "[E::%s] neither barcodes or sample IDs exist.\n", __func__); goto fail; }
        kputc('\n', s);
        if (output_headers(gs.out_vcf_cells, "wb", ks_str(s), ks_len(s)) < 0) {
            fprintf(stderr, "[E::%s] fail to write header to '%s'\n", __func__, gs.out_vcf_cells->fn);
            goto fail;
        }
    }
    /* set file modes. */
    gs.out_mtx_ad->fm = gs.out_mtx_dp->fm = gs.out_mtx_oth->fm = "ab";
    gs.out_vcf_base->fm = "ab";
    if (gs.is_genotype) { gs.out_vcf_cells->fm = "ab"; }
    /* run based on the mode of input. 
        Mode1: pileup a list of SNPs for a single BAM/SAM file with barcodes.
        Mode2: pileup whole chromosome(s) for one or multiple BAM/SAM files
        Mode3: pileup a list of SNPs for one or multiple BAM/SAM files with sample IDs.
    */
    if (gs.pos_list_file) {
        fprintf(stderr, "[I::%s] loading the VCF file for given SNPs ...\n", __func__);
        if (get_snplist(gs.pos_list_file, &gs.pl, &ret) <= 0 || ret < 0) {
            fprintf(stderr, "[E::%s] get SNP list from '%s' failed.\n", __func__, gs.pos_list_file);
            goto fail;
        }
        if (gs.barcodes) { 
            fprintf(stderr, "[I::%s] mode 1: fetch given SNPs in %d single cells.\n", __func__, gs.nbarcode); 
            if (run_mode1(&gs) < 0) { fprintf(stderr, "[E::%s] running mode 1 failed.\n", __func__); goto fail; } 
        } else { 
            fprintf(stderr, "[I::%s] mode 3: fetch given SNPs in %d bulk samples.\n", __func__, gs.nsid);
            if (run_mode3(&gs) < 0) { fprintf(stderr, "[E::%s] running mode 3 failed.\n", __func__); goto fail; } 
        }
    } else if (gs.chrom_all) { 
        if (gs.barcodes) { fprintf(stderr, "[I::%s] mode2: pileup %d whole chromosomes in %d single cells.\n", __func__, gs.nchrom, gs.nbarcode); }
        else { fprintf(stderr, "[I::%s] mode2: pileup %d whole chromosomes in one bulk sample.\n", __func__, gs.nchrom); }
        if (run_mode2(&gs) < 0) { fprintf(stderr, "[E::%s] running mode 2 failed.\n", __func__); goto fail; }
    } else {
        fprintf(stderr, "[E::%s] no proper mode to run, check input options.\n", __func__);
        print_usage(stderr);
        goto fail;
    }
    /* clean */
    ks_free(s); s = NULL;
    gll_setting_free(&gs);
    /* calc time spent */
    if (print_time) {
        time(&end_time);
        time_info = localtime(&end_time);
        strftime(time_str, 30, "%Y-%m-%d %H:%M:%S", time_info);
        fprintf(stderr, "[I::%s] end time: %s\n", __func__, time_str);
        fprintf(stderr, "[I::%s] time spent: %ld seconds.\n", __func__, end_time - start_time);
    }
    return 0;
  fail:
    if (s) { ks_free(s); }
    gll_setting_free(&gs);
    if (print_time) {
        time(&end_time);
        time_info = localtime(&end_time);
        strftime(time_str, 30, "%Y-%m-%d %H:%M:%S", time_info);
        fprintf(stderr, "[I::%s] end time: %s\n", __func__, time_str);
        fprintf(stderr, "[I::%s] time spent: %ld seconds.\n", __func__, end_time - start_time);
    }
    return 1;
}