#include "cones.h"
#include "linalg.h"
#include "scs.h"
#include "scs_blas.h" /* contains BLAS(X) macros and type info */
#include "util.h"

/* #define CONE_RATE (2) */
#define CONE_TOL (1e-9)
#define CONE_THRESH (1e-8)
#define EXP_CONE_MAX_ITERS (50)
#define POW_CONE_MAX_ITERS (20)

#ifdef USE_LAPACK
void BLAS(syevr)(const char *jobz, const char *range, const char *uplo,
                 blas_int *n, scs_float *a, blas_int *lda, scs_float *vl,
                 scs_float *vu, blas_int *il, blas_int *iu, scs_float *abstol,
                 blas_int *m, scs_float *w, scs_float *z, blas_int *ldz,
                 blas_int *isuppz, scs_float *work, blas_int *lwork,
                 blas_int *iwork, blas_int *liwork, blas_int *info);
void BLAS(syr)(const char *uplo, const blas_int *n, const scs_float *alpha,
               const scs_float *x, const blas_int *incx, scs_float *a,
               const blas_int *lda);
void BLAS(scal)(const blas_int *n, const scs_float *sa, scs_float *sx,
                const blas_int *incx);
scs_float BLAS(nrm2)(const blas_int *n, scs_float *x, const blas_int *incx);
#endif

// XXX add a comment about this
void SCS(set_rho_y_vec)(const ScsCone *k, scs_float scale, scs_float *rho_y_vec,
                        scs_int m) {
  scs_int i, count = 0;
  /* f cone */
  for (i = 0; i < k->f; ++i) {
    rho_y_vec[i] = 1000. * scale;
  }
  count += k->f + k->l;

  //if (k->bsize > 0) {
  //  rho_y_vec[count] = 1000. * scale; /* first element of b cone is 1 */
  //  count += 1; /* only the first element */
  //}
  /* others */
  for (i = count; i < m; ++i) {
    rho_y_vec[i] = scale;
  }
  /* Note, if updating this to use different scales for other cones (e.g. box)
   * then you must be careful to also include the effect of the rho_y_vec
   * in the actual projection operator.
   */
}

static scs_int get_sd_cone_size(scs_int s) { return (s * (s + 1)) / 2; }

/*
 * boundaries will contain array of indices of rows of A corresponding to
 * cone boundaries, boundaries[0] is starting index for cones of size strictly
 * larger than 1, boundaries malloc-ed here so should be freed.
 * Returns total length of cones m.
 */
static scs_int set_cone_boundaries(const ScsCone *k, ScsConeWork *c) {
  scs_int i, s_cone_sz, count = 0, m = 0;
  c->cone_boundaries_len = 1 + k->qsize + k->ssize + k->ed + k->ep + k->psize;
  scs_int *b = (scs_int *)scs_calloc(c->cone_boundaries_len, sizeof(scs_int));
  b[count] = k->f + k->l + k->bsize; /* cones can be scaled independently */
  m += k->f + k->l + k->bsize;
  count += 1; /* started at 0 now move to first entry */
  for (i = 0; i < k->qsize; ++i) {
    b[count + i] = k->q[i];
    m += k->q[i];
  }
  count += k->qsize;
  for (i = 0; i < k->ssize; ++i) {
    s_cone_sz = get_sd_cone_size(k->s[i]);
    b[count + i] = s_cone_sz;
    m += s_cone_sz;
  }
  count += k->ssize; /* add ssize here not ssize * (ssize + 1) / 2 */
  /* exp cones */
  for (i = 0; i < k->ep + k->ed; ++i) {
    b[count + i] = 3;
  }
  count += k->ep + k->ed;
  m += 3 * (k->ep + k->ed);
  /* power cones */
  for (i = 0; i < k->psize; ++i) {
    b[count + i] = 3;
  }
  count += k->psize;
  m += 3 * k->psize;
  /* other cones */
  c->cone_boundaries = b;
  return m;
}

static scs_int get_full_cone_dims(const ScsCone *k) {
  scs_int i, c = k->f + k->l + k->bsize;
  if (k->qsize) {
    for (i = 0; i < k->qsize; ++i) {
      c += k->q[i];
    }
  }
  if (k->ssize) {
    for (i = 0; i < k->ssize; ++i) {
      c += get_sd_cone_size(k->s[i]);
    }
  }
  if (k->ed) {
    c += 3 * k->ed;
  }
  if (k->ep) {
    c += 3 * k->ep;
  }
  if (k->p) {
    c += 3 * k->psize;
  }
  return c;
}

scs_int SCS(validate_cones)(const ScsData *d, const ScsCone *k) {
  scs_int i;
  if (get_full_cone_dims(k) != d->m) {
    scs_printf("cone dimensions %li not equal to num rows in A = m = %li\n",
               (long)get_full_cone_dims(k), (long)d->m);
    return -1;
  }
  if (k->f && k->f < 0) {
    scs_printf("free cone dimension error\n");
    return -1;
  }
  if (k->l && k->l < 0) {
    scs_printf("lp cone dimension error\n");
    return -1;
  }
  if (k->bsize) {
    if (k->bsize < 0) {
      scs_printf("box cone dimension error\n");
      return -1;
    }
    for (i = 0; i < k->bsize - 1; ++i) {
      if (k->bl[i] > k->bu[i]) {
        scs_printf("infeasible: box lower bound larger than upper bound\n");
        return -1;
      }
    }
  }
  if (k->qsize && k->q) {
    if (k->qsize < 0) {
      scs_printf("soc cone dimension error\n");
      return -1;
    }
    for (i = 0; i < k->qsize; ++i) {
      if (k->q[i] < 0) {
        scs_printf("soc cone dimension error\n");
        return -1;
      }
    }
  }
  if (k->ssize && k->s) {
    if (k->ssize < 0) {
      scs_printf("sd cone dimension error\n");
      return -1;
    }
    for (i = 0; i < k->ssize; ++i) {
      if (k->s[i] < 0) {
        scs_printf("sd cone dimension error\n");
        return -1;
      }
    }
  }
  if (k->ed && k->ed < 0) {
    scs_printf("ep cone dimension error\n");
    return -1;
  }
  if (k->ep && k->ep < 0) {
    scs_printf("ed cone dimension error\n");
    return -1;
  }
  if (k->psize && k->p) {
    if (k->psize < 0) {
      scs_printf("power cone dimension error\n");
      return -1;
    }
    for (i = 0; i < k->psize; ++i) {
      if (k->p[i] < -1 || k->p[i] > 1) {
        scs_printf("power cone error, values must be in [-1,1]\n");
        return -1;
      }
    }
  }
  return 0;
}

void SCS(finish_cone)(ScsConeWork *c) {
#ifdef USE_LAPACK
  if (c->Xs) {
    scs_free(c->Xs);
  }
  if (c->Z) {
    scs_free(c->Z);
  }
  if (c->e) {
    scs_free(c->e);
  }
  if (c->work) {
    scs_free(c->work);
  }
  if (c->iwork) {
    scs_free(c->iwork);
  }
#endif
  if (c->s) {
    scs_free(c->s);
  }
  if (c->cone_boundaries) {
    scs_free(c->cone_boundaries);
  }
  if (c) {
    scs_free(c);
  }
}

char *SCS(get_cone_header)(const ScsCone *k) {
  char *tmp = (char *)scs_malloc(sizeof(char) * 512);
  scs_int i, soc_vars, soc_blks, sd_vars, sd_blks;
  sprintf(tmp, "cones: ");
  if (k->f) {
    sprintf(tmp + strlen(tmp), "\t  f: primal zero / dual free vars: %li\n",
            (long)k->f);
  }
  if (k->l) {
    sprintf(tmp + strlen(tmp), "\t  l: linear vars: %li\n", (long)k->l);
  }
  if (k->bsize) {
    sprintf(tmp + strlen(tmp), "\t  b: box cone vars: %li\n", (long)k->bsize);
  }
  soc_vars = 0;
  soc_blks = 0;
  if (k->qsize && k->q) {
    soc_blks = k->qsize;
    for (i = 0; i < k->qsize; i++) {
      soc_vars += k->q[i];
    }
    sprintf(tmp + strlen(tmp), "\t  q: soc vars: %li, soc blks: %li\n",
            (long)soc_vars, (long)soc_blks);
  }
  sd_vars = 0;
  sd_blks = 0;
  if (k->ssize && k->s) {
    sd_blks = k->ssize;
    for (i = 0; i < k->ssize; i++) {
      sd_vars += get_sd_cone_size(k->s[i]);
    }
    sprintf(tmp + strlen(tmp), "\t  s: sd vars: %li, sd blks: %li\n",
            (long)sd_vars, (long)sd_blks);
  }
  if (k->ep || k->ed) {
    sprintf(tmp + strlen(tmp), "\t  e: exp vars: %li, dual exp vars: %li\n",
            (long)(3 * k->ep), (long)(3 * k->ed));
  }
  if (k->psize && k->p) {
    sprintf(tmp + strlen(tmp), "\t  p: primal + dual power vars: %li\n",
            (long)(3 * k->psize));
  }
  return tmp;
}

static scs_int is_simple_semi_definite_cone(scs_int *s, scs_int ssize) {
  scs_int i;
  for (i = 0; i < ssize; i++) {
    if (s[i] > 2) {
      return 0; /* false */
    }
  }
  return 1; /* true */
}

static scs_float exp_newton_one_d(scs_float rho, scs_float y_hat,
                                  scs_float z_hat, scs_float w){
  scs_float t = MAX(w - z_hat, MAX(-z_hat, 1e-9));
  scs_float f, fp;
  scs_int i;
  for (i = 0; i < EXP_CONE_MAX_ITERS; ++i) {
    f = t * (t + z_hat) / rho / rho - y_hat / rho + log(t / rho) + 1;
    fp = (2 * t + z_hat) / rho / rho + 1 / t;

    t = t - f / fp;

    if (t <= -z_hat) {
      t = -z_hat;
      break;
    } else if (t <= 0) {
      t = 0;
      break;
    } else if (SQRTF(f * f / fp) < CONE_TOL) {
      break;
    }
  }
#if EXTRA_VERBOSE > 1
  if (i == EXP_CONE_MAX_ITERS) {
    scs_printf("warning: exp cone newton step took maximum %i iters\n", (int)i);
    scs_printf("rho=%1.5e; y_hat=%1.5e; z_hat=%1.5e; w=%1.5e\n", rho, y_hat,
                z_hat, w);
  }
#endif
  return t + z_hat;
}

static void exp_solve_for_x_with_rho(scs_float *v, scs_float *x,
                                     scs_float rho, scs_float w) {
  x[2] = exp_newton_one_d(rho, v[1], v[2], w);
  x[1] = (x[2] - v[2]) * x[2] / rho;
  x[0] = v[0] - rho;
}

static scs_float exp_calc_grad(scs_float *v, scs_float *x, scs_float rho,
                               scs_float w) {
  exp_solve_for_x_with_rho(v, x, rho, w);
  if (x[1] <= 1e-12) {
    return x[0];
  }
  return x[0] + x[1] * log(x[1] / x[2]);
}

static void exp_get_rho_ub(scs_float *v, scs_float *x, scs_float *ub,
                           scs_float *lb) {
  *lb = 0;
  *ub = 0.125;
  while (exp_calc_grad(v, x, *ub, v[1]) > 0) {
    *lb = *ub;
    (*ub) *= 2;
  }
}

/* project onto the exponential cone, v has dimension *exactly* 3 */
static scs_int proj_exp_cone(scs_float *v) {
  scs_int i;
  scs_float ub, lb, rho, g, x[3];
  scs_float r = v[0], s = v[1], t = v[2];
  scs_float tol = CONE_TOL; /* iter < 0 ? CONE_TOL : MAX(CONE_TOL, 1 /
                               POWF((iter + 1), CONE_RATE)); */

  /* v in cl(Kexp) */
  if ((s * exp(r / s) - t <= CONE_THRESH && s > 0) ||
      (r <= 0 && s == 0 && t >= 0)) {
    return 0;
  }

  /* -v in Kexp^* */
  if ((-r < 0 && r * exp(s / r) + exp(1) * t <= CONE_THRESH) ||
      (-r == 0 && -s >= 0 && -t >= 0)) {
    memset(v, 0, 3 * sizeof(scs_float));
    return 0;
  }

  /* special case with analytical solution */
  if (r < 0 && s < 0) {
    v[1] = 0.0;
    v[2] = MAX(v[2], 0);
    return 0;
  }

  /* iterative procedure to find projection, bisects on dual variable: */
  exp_get_rho_ub(v, x, &ub, &lb); /* get starting upper and lower bounds */
  for (i = 0; i < EXP_CONE_MAX_ITERS; ++i) {
    rho = (ub + lb) / 2;          /* halfway between upper and lower bounds */
    g = exp_calc_grad(v, x, rho, x[1]); /* calculates gradient wrt dual var */
    if (g > 0) {
      lb = rho;
    } else {
      ub = rho;
    }
    if (ub - lb < tol) {
      break;
    }
  }
#if EXTRA_VERBOSE > 25
  scs_printf("exponential cone proj iters %i\n", (int)i);
#endif
  v[0] = x[0];
  v[1] = x[1];
  v[2] = x[2];
  return 0;
}

static scs_int set_up_sd_cone_work_space(ScsConeWork *c, const ScsCone *k) {
#ifdef USE_LAPACK
  scs_int i;
  blas_int n_max = 0;
  scs_float eig_tol = 1e-8;
  blas_int neg_one = -1;
  blas_int m = 0;
  blas_int info = 0;
  scs_float wkopt = 0.0;
#if EXTRA_VERBOSE > 0
#define _STR_EXPAND(tok) #tok
#define _STR(tok) _STR_EXPAND(tok)
  scs_printf("BLAS(func) = '%s'\n", _STR(BLAS(func)));
#endif
  /* eigenvector decomp workspace */
  for (i = 0; i < k->ssize; ++i) {
    if (k->s[i] > n_max) {
      n_max = (blas_int)k->s[i];
    }
  }
  c->Xs = (scs_float *)scs_calloc(n_max * n_max, sizeof(scs_float));
  c->Z = (scs_float *)scs_calloc(n_max * n_max, sizeof(scs_float));
  c->e = (scs_float *)scs_calloc(n_max, sizeof(scs_float));
  c->liwork = 0;

  BLAS(syevr)
  ("Vectors", "All", "Lower", &n_max, c->Xs, &n_max, SCS_NULL, SCS_NULL,
   SCS_NULL, SCS_NULL, &eig_tol, &m, c->e, c->Z, &n_max, SCS_NULL, &wkopt,
   &neg_one, &(c->liwork), &neg_one, &info);

  if (info != 0) {
    scs_printf("FATAL: syevr failure, info = %li\n", (long)info);
    return -1;
  }
  c->lwork = (blas_int)(wkopt + 0.01); /* 0.01 for int casting safety */
  c->work = (scs_float *)scs_calloc(c->lwork, sizeof(scs_float));
  c->iwork = (blas_int *)scs_calloc(c->liwork, sizeof(blas_int));

  if (!c->Xs || !c->Z || !c->e || !c->work || !c->iwork) {
    return -1;
  }
  return 0;
#else
  scs_printf(
      "FATAL: Cannot solve SDPs with > 2x2 matrices without linked "
      "blas+lapack libraries\n");
  scs_printf(
      "Install blas+lapack and re-compile SCS with blas+lapack library "
      "locations\n");
  return -1;
#endif
}

ScsConeWork *SCS(init_cone)(const ScsCone *k) {
  ScsConeWork *c = (ScsConeWork *)scs_calloc(1, sizeof(ScsConeWork));
  c->m = set_cone_boundaries(k, c);
  c->s = (scs_float *)scs_calloc(c->m, sizeof(scs_float));
  if (k->ssize && k->s) {
    if (!is_simple_semi_definite_cone(k->s, k->ssize) &&
        set_up_sd_cone_work_space(c, k) < 0) {
      SCS(finish_cone)(c);
      return SCS_NULL;
    }
  }
  return c;
}

static scs_int project_2x2_sdc(scs_float *X) {
  scs_float a, b, d, l1, l2, x1, x2, rad;
  scs_float sqrt2 = SQRTF(2.0);
  a = X[0];
  b = X[1] / sqrt2;
  d = X[2];

  if (ABS(b) < 1e-6) { /* diagonal matrix */
    X[0] = MAX(a, 0);
    X[1] = 0;
    X[2] = MAX(d, 0);
    return 0;
  }

  rad = SQRTF((a - d) * (a - d) + 4 * b * b);
  /* l1 >= l2 always, since rad >= 0 */
  l1 = 0.5 * (a + d + rad);
  l2 = 0.5 * (a + d - rad);

#if EXTRA_VERBOSE > 0
  scs_printf(
      "2x2 SD: a = %4f, b = %4f, (X[1] = %4f, X[2] = %4f), d = %4f, "
      "rad = %4f, l1 = %4f, l2 = %4f\n",
      a, b, X[1], X[2], d, rad, l1, l2);
#endif

  if (l2 >= 0) { /* both eigs positive already */
    return 0;
  }
  if (l1 <= 0) { /* both eigs negative, set to 0 */
    X[0] = 0;
    X[1] = 0;
    X[2] = 0;
    return 0;
  }

  /* l1 pos, l2 neg */
  x1 = 1 / SQRTF(1 + (l1 - a) * (l1 - a) / b / b);
  x2 = x1 * (l1 - a) / b;

  X[0] = l1 * x1 * x1;
  X[1] = (l1 * x1 * x2) * sqrt2;
  X[2] = l1 * x2 * x2;
  return 0;
}

/* size of X is get_sd_cone_size(n) */
static scs_int proj_semi_definite_cone(scs_float *X, const scs_int n,
                                       ScsConeWork *c) {
/* project onto the positive semi-definite cone */
#ifdef USE_LAPACK
  scs_int i;
  blas_int one = 1;
  blas_int m = 0;
  blas_int nb = (blas_int)n;
  blas_int nb_plus_one = (blas_int)(n + 1);
  blas_int cone_sz = (blas_int)(get_sd_cone_size(n));

  scs_float sqrt2 = SQRTF(2.0);
  scs_float sqrt2Inv = 1.0 / sqrt2;
  scs_float *Xs = c->Xs;
  scs_float *Z = c->Z;
  scs_float *e = c->e;
  scs_float *work = c->work;
  blas_int *iwork = c->iwork;
  blas_int lwork = c->lwork;
  blas_int liwork = c->liwork;

  scs_float eig_tol = CONE_TOL; /* iter < 0 ? CONE_TOL : MAX(CONE_TOL, 1 /
                                  POWF(iter + 1, CONE_RATE)); */
  scs_float zero = 0.0;
  blas_int info = 0;
  scs_float vupper = 0.0;
#endif
  if (n == 0) {
    return 0;
  }
  if (n == 1) {
    if (X[0] < 0.0) {
      X[0] = 0.0;
    }
    return 0;
  }
  if (n == 2) {
    return project_2x2_sdc(X);
  }
#ifdef USE_LAPACK

  memset(Xs, 0, n * n * sizeof(scs_float));
  /* expand lower triangular matrix to full matrix */
  for (i = 0; i < n; ++i) {
    memcpy(&(Xs[i * (n + 1)]), &(X[i * n - ((i - 1) * i) / 2]),
           (n - i) * sizeof(scs_float));
  }
  /*
     rescale so projection works, and matrix norm preserved
     see http://www.seas.ucla.edu/~vandenbe/publications/mlbook.pdf pg 3
   */
  /* scale diags by sqrt(2) */
  BLAS(scal)(&nb, &sqrt2, Xs, &nb_plus_one); /* not n_squared */

  /* max-eig upper bounded by frobenius norm */
  /* mult by factor to make sure is upper bound */
  vupper = 1.1 * sqrt2 * BLAS(nrm2)(&cone_sz, X, &one);
  vupper = MAX(vupper, 0.01);
#if EXTRA_VERBOSE > 0
  SCS(print_array)(Xs, n * n, "Xs");
  SCS(print_array)(X, get_sd_cone_size(n), "X");
#endif
  /* Solve eigenproblem, reuse workspaces */
  BLAS(syevr)
  ("Vectors", "VInterval", "Lower", &nb, Xs, &nb, &zero, &vupper, SCS_NULL,
   SCS_NULL, &eig_tol, &m, e, Z, &nb, SCS_NULL, work, &lwork, iwork, &liwork,
   &info);
#if EXTRA_VERBOSE > 0
  if (info != 0) {
    scs_printf("WARN: LAPACK syevr error, info = %i\n", info);
  }
  scs_printf("syevr input parameter dump:\n");
  scs_printf("nb = %li\n", (long)nb);
  scs_printf("lwork = %li\n", (long)lwork);
  scs_printf("liwork = %li\n", (long)liwork);
  scs_printf("vupper = %f\n", vupper);
  scs_printf("eig_tol = %e\n", eig_tol);
  SCS(print_array)(e, m, "e");
  SCS(print_array)(Z, m * n, "Z");
#endif
  if (info < 0) {
    return -1;
  }

  memset(Xs, 0, n * n * sizeof(scs_float));
  for (i = 0; i < m; ++i) {
    scs_float a = e[i];
    BLAS(syr)("Lower", &nb, &a, &(Z[i * n]), &one, Xs, &nb);
  }
  /* scale diags by 1/sqrt(2) */
  BLAS(scal)(&nb, &sqrt2Inv, Xs, &nb_plus_one); /* not n_squared */
  /* extract just lower triangular matrix */
  for (i = 0; i < n; ++i) {
    memcpy(&(X[i * n - ((i - 1) * i) / 2]), &(Xs[i * (n + 1)]),
           (n - i) * sizeof(scs_float));
  }

#if EXTRA_VERBOSE > 0
  SCS(print_array)(Xs, n * n, "Xs");
  SCS(print_array)(X, get_sd_cone_size(n), "X");
#endif

#else
  scs_printf(
      "FAILURE: solving SDP with > 2x2 matrices, but no blas/lapack "
      "libraries were linked!\n");
  scs_printf("SCS will return nonsense!\n");
  SCS(scale_array)(X, NAN, n);
  return -1;
#endif
  return 0;
}

static scs_float pow_calc_x(scs_float r, scs_float xh, scs_float rh,
                            scs_float a) {
  scs_float x = 0.5 * (xh + SQRTF(xh * xh + 4 * a * (rh - r) * r));
  return MAX(x, 1e-12);
}

static scs_float pow_calcdxdr(scs_float x, scs_float xh, scs_float rh,
                              scs_float r, scs_float a) {
  return a * (rh - 2 * r) / (2 * x - xh);
}

static scs_float pow_calc_f(scs_float x, scs_float y, scs_float r,
                            scs_float a) {
  return POWF(x, a) * POWF(y, (1 - a)) - r;
}

static scs_float pow_calc_fp(scs_float x, scs_float y, scs_float dxdr,
                             scs_float dydr, scs_float a) {
  return POWF(x, a) * POWF(y, (1 - a)) * (a * dxdr / x + (1 - a) * dydr / y) -
         1;
}

/* project onto { (t, x) | t * l <= x <= t * u, t >= 0 }, Newton's method on t
   tx = [t; x], total length = bsize
   uses Moreau since \Pi_K*(tx) = \Pi_K(-tx) + tx
   D contains equilibration scaling matrix, can be SCS_NULL
*/
#define MAX_BOX_VAL (1e15)
static void proj_box_cone(scs_float *tx, const scs_float *bl,
                          const scs_float *bu, scs_int bsize,
                          const scs_float * D) {
  scs_float gt, ht, dl, du, t_prev, t = MAX(tx[0], 0.), *x = &(tx[1]);
  scs_int iter, j, max_iter = 100;
#if EXTRA_VERBOSE > 10
  SCS(print_array)(bu, bsize - 1, "u");
  SCS(print_array)(bl, bsize - 1, "l");
  SCS(print_array)(tx, bsize, "tx");
#endif
  /* should only require about 5 iterations maximum */
  for (iter = 0; iter < max_iter; iter++) {
    t_prev = t;
    gt = t - tx[0]; /* gradient */
    ht = 1.; /* hessian */
    for (j = 0; j < bsize - 1; j++) {
      if (bu[j] < MAX_BOX_VAL) { /* if > than this consider to be infinity */
        du = D ? D[j+1] * bu[j] / D[0] : bu[j];
        if (x[j] > t * du) {
          gt += (t * du - x[j]) * du; /* gradient */
          ht += du * du; /* hessian */
          continue;
        }
      }
      if (bl[j] > -MAX_BOX_VAL) { /* if < than this consider to be -infinity */
        dl = D ? D[j+1] * bl[j] / D[0] : bl[j];
        if (x[j] < t * dl) {
          gt += (t * dl - x[j]) * dl; /* gradient */
          ht += dl * dl; /* hessian */
        }
      }
    }
#if EXTRA_VERBOSE > -1
    scs_printf("t_new %4f, t_prev %4f, gt %4f, ht %4f\n",
      MAX(t - gt / MAX(ht, 1e-8), 0.), t, gt, ht);
#endif
    t = MAX(t - gt / MAX(ht, 1e-8), 0.); /* newton step */
    if (ABS(gt / (ht + 1e-6)) < 1e-12 || ABS(t - t_prev) < 1e-12) { break; }
  }
  if (iter == max_iter) {
    scs_printf("warning: box cone proj took maximum %i iters\n", (int)iter);
  }
  for (j = 0; j < bsize - 1; j++) {
    if (bu[j] < MAX_BOX_VAL) { /* if > than this consider to be infinity */
      du = D ? D[j+1] * bu[j] / D[0] : bu[j];
      if (x[j] > t * du) {
        x[j] = t * du;
        continue;
      }
    }
    if (bl[j] > -MAX_BOX_VAL) {  /* if < than this consider to be -infinity */
      dl = D ? D[j+1] * bl[j] / D[0] : bl[j];
      if (x[j] < t * dl) {
        x[j] = t * dl;
        continue;
      }
    }
    /* x[j] unchanged otherwise */
  }
  tx[0] = t;
#if EXTRA_VERBOSE > -1
  scs_printf("box cone iters %i\n", (int)iter);
#endif
#if EXTRA_VERBOSE > 10
  SCS(print_array)(tx, bsize, "tx_+");
#endif
  return;
}

/* project onto SOC of size q*/
static void proj_soc(scs_float * x, scs_int q) {
  if (q == 0) {
    return;
  }
  if (q == 1) {
    x[0] = MAX(x[0], 0.);
    return;
  }
  scs_float v1 = x[0];
  scs_float s = SCS(norm)(&(x[1]), q - 1);
  scs_float alpha = (s + v1) / 2.0;

  if (s <= v1) {
    return;
  } else if (s <= -v1) {
    memset(&(x[0]), 0, q * sizeof(scs_float));
  } else {
    x[0] = alpha;
    SCS(scale_array)(&(x[1]), alpha / s, q - 1);
  }
}

static void proj_power_cone(scs_float *v, scs_float a) {
  scs_float xh = v[0], yh = v[1], rh = ABS(v[2]);
  scs_float x = 0.0, y = 0.0, r;
  scs_int i;
  /* v in K_a */
  if (xh >= 0 && yh >= 0 &&
      CONE_THRESH + POWF(xh, a) * POWF(yh, (1 - a)) >= rh) {
    return;
  }

  /* -v in K_a^* */
  if (xh <= 0 && yh <= 0 &&
      CONE_THRESH + POWF(-xh, a) * POWF(-yh, 1 - a) >=
          rh * POWF(a, a) * POWF(1 - a, 1 - a)) {
    v[0] = v[1] = v[2] = 0;
    return;
  }

  r = rh / 2;
  for (i = 0; i < POW_CONE_MAX_ITERS; ++i) {
    scs_float f, fp, dxdr, dydr;
    x = pow_calc_x(r, xh, rh, a);
    y = pow_calc_x(r, yh, rh, 1 - a);

    f = pow_calc_f(x, y, r, a);
    if (ABS(f) < CONE_TOL) {
      break;
    }

    dxdr = pow_calcdxdr(x, xh, rh, r, a);
    dydr = pow_calcdxdr(y, yh, rh, r, (1 - a));
    fp = pow_calc_fp(x, y, dxdr, dydr, a);

    r = MAX(r - f / fp, 0);
    r = MIN(r, rh);
  }
  v[0] = x;
  v[1] = y;
  v[2] = (v[2] < 0) ? -(r) : (r);
}

static scs_int proj_cone(scs_float *x, const ScsCone *k, ScsConeWork *c,
                  const scs_float *warm_start, ScsScaling *scaling,
                  scs_int iter) {
  scs_int i;
  scs_int count = k->f;
  scs_float *D = scaling ? scaling->D : SCS_NULL;

  if (k->l) {
    /* project onto positive orthant */
    for (i = count; i < count + k->l; ++i) {
      x[i] = MAX(x[i], 0.0);
    }
    count += k->l;
  }

  if (k->bsize) {
    /* project onto this cone using Moreau */
    proj_box_cone(&(x[count]), k->bl, k->bu, k->bsize, D ? &(D[count]) : D);
    count += k->bsize;
  }

  if (k->qsize && k->q) {
    for (i = 0; i < k->qsize; ++i) {
      proj_soc(&(x[count]), k->q[i]);
      count += k->q[i];
    }
  }

  if (k->ssize && k->s) {
    /* project onto PSD cone */
    for (i = 0; i < k->ssize; ++i) {
      if (k->s[i] == 0) {
        continue;
      }
      if (proj_semi_definite_cone(&(x[count]), k->s[i], c) < 0) {
        return -1;
      }
      count += get_sd_cone_size(k->s[i]);
    }
  }

  if (k->ep) {
    /*
     * exponential cone is not self dual, if s \in K
     * then y \in K^* and so if K is the primal cone
     * here we project onto K^*, via Moreau
     * \Pi_C^*(y) = y + \Pi_C(-y)
     */
#ifdef _OPENMP
#pragma omp parallel for private(r, s, t, idx)
#endif
    for (i = 0; i < k->ep; ++i) {
      proj_exp_cone(&(x[count + 3 * i]));
    }
    count += 3 * k->ep;
#if EXTRA_VERBOSE > 0
    scs_printf("EP proj time: %1.2es\n", SCS(tocq)(&proj_timer) / 1e3);
    SCS(tic)(&proj_timer);
#endif
  }

  if (k->ed) { /* dual exponential cone */
    /*
     * exponential cone is not self dual, if s \in K
     * then y \in K^* and so if K is the primal cone
     * here we project onto K^*, via Moreau
     * \Pi_C^*(y) = y + \Pi_C(-y)
     */
    scs_int idx;
    scs_float r, s, t;
    SCS(scale_array)(&(x[count]), -1, 3 * k->ed); /* x = -x; */
#ifdef _OPENMP
#pragma omp parallel for
#endif
    for (i = 0; i < k->ed; ++i) {
      idx = count + 3 * i;
      r = x[idx];
      s = x[idx + 1];
      t = x[idx + 2];

      proj_exp_cone(&(x[idx]));

      x[idx] -= r;
      x[idx + 1] -= s;
      x[idx + 2] -= t;
    }
    count += 3 * k->ed;
  }

  if (k->psize && k->p) {
    scs_float v[3];
    scs_int idx;
    /* don't use openmp for power cone
    ifdef _OPENMP
    pragma omp parallel for private(v, idx)
    endif
    */
    for (i = 0; i < k->psize; ++i) {
      idx = count + 3 * i;
      if (k->p[i] >= 0) {
        /* primal power cone */
        proj_power_cone(&(x[idx]), k->p[i]);
      } else {
        /* dual power cone, using Moreau */
        v[0] = -x[idx];
        v[1] = -x[idx + 1];
        v[2] = -x[idx + 2];

        proj_power_cone(v, -k->p[i]);

        x[idx] += v[0];
        x[idx + 1] += v[1];
        x[idx + 2] += v[2];
      }
    }
    count += 3 * k->psize;
#if EXTRA_VERBOSE > 0
    scs_printf("Power cone proj time: %1.2es\n", SCS(tocq)(&proj_timer) / 1e3);
    SCS(tic)(&proj_timer);
#endif
  }
  /* project onto OTHER cones */
  return 0;
}

/* outward facing cone projection routine, iter is outer algorithm iteration, if
   iter < 0 then iter is ignored
   warm_start contains guess of projection (can be set to SCS_NULL)
   D contains scaling matrix, some cones can make use of it
*/
scs_int SCS(proj_dual_cone)(scs_float *x, const ScsCone *k, ScsConeWork *c,
                            const scs_float *warm_start, ScsScaling *scaling,
                            scs_int iter) {
  /* copy x, s = x */
  scs_int status;
  memcpy(c->s, x, sizeof(scs_float) * c->m);
  /* negate x, -x */
  SCS(scale_array)(x, -1, c->m);
  /* project x onto cone, x = Pi_K(-x) */
  status = proj_cone(x, k, c, warm_start, scaling, iter);
  /* return Pi_K*(x) = s + Pi_K(-x) */
  SCS(add_scaled_array)(x, c->s, c->m, 1.0);
  return status;
}


