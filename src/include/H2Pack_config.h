#ifndef __H2PACK_CONFIG_H__
#define __H2PACK_CONFIG_H__

// Parameters used in H2Pack

#define DOUBLE_SIZE  8
#define FLOAT_SIZE   4

#define DTYPE_SIZE   DOUBLE_SIZE  // Matrix data type: double or float

#if DTYPE_SIZE == DOUBLE_SIZE     // Functions for double data type
#define DTYPE       double        // Data type
#define DABS        fabs          // Abs function
#define DFLOOR      floor         // Floor function
#define DSQRT       sqrt          // Sqrt function
#define CBLAS_NRM2  cblas_dnrm2   // CBLAS 2-norm function
#define CBLAS_SCAL  cblas_dscal   // CBLAS vector scaling function
#define CBLAS_GEMV  cblas_dgemv   // CBLAS matrix-vector multiplication
#define CBLAS_GER   cblas_dger    // CBLAS matrix rank-1 update
#define CBLAS_TRSM  cblas_dtrsm   // CBLAS triangle solve
#endif

#if DTYPE_SIZE == FLOAT_SIZE      // Functions for float data type
#define DTYPE       float
#define DABS        fabsf
#define DFLOOR      floorf
#define DSQRT       sqrtf    
#define CBLAS_NRM2  cblas_snrm2
#define CBLAS_SCAL  cblas_sscal
#define CBLAS_GEMV  cblas_sgemv
#define CBLAS_GER   cblas_sger
#define CBLAS_TRSM  cblas_strsm
#endif

#define QR_RANK     0   // Partial QR stop criteria: maximum rank
#define QR_REL_NRM  1   // Partial QR stop criteria: maximum relative column 2-norm
#define QR_ABS_NRM  2   // Partial QR stop criteria: maximum absolute column 2-norm

#define ALIGN_SIZE  64            // Memory allocation alignment
#define ALPHA_H2    0.999999      // Admissible coefficient, == 1 here

#endif
