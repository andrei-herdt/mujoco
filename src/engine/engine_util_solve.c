// Copyright 2021 DeepMind Technologies Limited
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "engine/engine_util_solve.h"

#include <math.h>
#include <string.h>

#include <mujoco/mjdata.h>
#include <mujoco/mjmodel.h>
#include "engine/engine_io.h"
#include "engine/engine_macro.h"
#include "engine/engine_util_blas.h"
#include "engine/engine_util_errmem.h"
#include "engine/engine_util_sparse.h"
#include "engine/engine_util_spatial.h"

//---------------------------- dense Cholesky ------------------------------------------------------

// Cholesky decomposition: mat = L*L'; return 'rank'
int mju_cholFactor(mjtNum* mat, int n, mjtNum mindiag) {
  int rank = n;
  mjtNum tmp;

  // in-place Cholesky factorization
  for (int j=0; j<n; j++) {
    // compute new diagonal
    tmp = mat[j*(n+1)];
    if (j) {
      tmp -= mju_dot(mat+j*n, mat+j*n, j);
    }

    // correct diagonal values below threshold
    if (tmp<mindiag) {
      tmp = mindiag;
      rank--;
    }

    // save diagonal
    mat[j*(n+1)] = mju_sqrt(tmp);

    // process off-diagonal entries
    tmp = 1/mat[j*(n+1)];
    for (int i=j+1; i<n; i++) {
      mat[i*n+j] = (mat[i*n+j] - mju_dot(mat+i*n, mat+j*n, j)) * tmp;
    }
  }

  return rank;
}



// Cholesky solve
void mju_cholSolve(mjtNum* res, const mjtNum* mat, const mjtNum* vec, int n) {
  // copy if source and destination are different
  if (res!=vec) {
    mju_copy(res, vec, n);
  }

  // forward substitution: solve L*res = vec
  for (int i=0; i<n; i++) {
    if (i) {
      res[i] -= mju_dot(mat+i*n, res, i);
    }

    // diagonal
    res[i] /= mat[i*(n+1)];
  }

  // backward substitution: solve L'*res = res
  for (int i=n-1; i>=0; i--) {
    if (i<n-1) {
      for (int j=i+1; j<n; j++) {
        res[i] -= mat[j*n+i] * res[j];
      }
    }
    // diagonal
    res[i] /= mat[i*(n+1)];
  }
}



// Cholesky rank-one update: L*L' +/- x*x'; return rank
int mju_cholUpdate(mjtNum* mat, mjtNum* x, int n, int flg_plus) {
  int rank = n;
  mjtNum r, c, cinv, s, Lkk, tmp;

  for (int k=0; k<n; k++) {
    if (x[k]) {
      // prepare constants
      Lkk = mat[k*(n+1)];
      tmp = Lkk*Lkk + (flg_plus ? x[k]*x[k] : -x[k]*x[k]);
      if (tmp<mjMINVAL) {
        tmp = mjMINVAL;
        rank--;
      }
      r = mju_sqrt(tmp);
      c = r / Lkk;
      cinv = 1 / c;
      s = x[k] / Lkk;

      // update diagonal
      mat[k*(n+1)] = r;

      // update mat
      if (flg_plus)
        for (int i=k+1; i<n; i++) {
          mat[i*n+k] = (mat[i*n+k] + s*x[i])*cinv;
        } else
        for (int i=k+1; i<n; i++) {
          mat[i*n+k] = (mat[i*n+k] - s*x[i])*cinv;
        }

      // update x
      for (int i=k+1; i<n; i++) {
        x[i] = c*x[i] - s*mat[i*n+k];
      }
    }
  }

  return rank;
}



//---------------------------- sparse Cholesky -----------------------------------------------------

// sparse reverse-order Cholesky decomposition: mat = L'*L; return 'rank'
//  mat must have uncompressed layout; rownnz is modified to end at diagonal
int mju_cholFactorSparse(mjtNum* mat, int n, mjtNum mindiag,
                         int* rownnz, int* rowadr, int* colind,
                         mjData* d) {
  int rank = n;

  mjMARKSTACK;
  int* buf_ind = (int*) mj_stackAlloc(d, n);
  mjtNum* sparse_buf = mj_stackAlloc(d, n);

  // shrink rows so that rownnz ends at diagonal
  for (int r=0; r<n; r++) {
    // shrink
    while (rownnz[r]>0 && colind[rowadr[r]+rownnz[r]-1]>r) {
      rownnz[r]--;
    }

    // check
    if (rownnz[r]==0 || colind[rowadr[r]+rownnz[r]-1]!=r) {
      mju_error("Matrix must have non-zero diagonal in mju_cholFactorSparse");
    }
  }

  // backpass over rows
  for (int r=n-1; r>=0; r--) {
    // get rownnz and rowadr for row r
    int nnz = rownnz[r], adr = rowadr[r];

    // update row r diagonal
    mjtNum tmp = mat[adr+nnz-1];
    if (tmp<mindiag) {
      tmp = mindiag;
      rank--;
    }
    mat[adr+nnz-1] = mju_sqrt(tmp);
    tmp = 1/mat[adr+nnz-1];

    // update row r before diagonal
    for (int i=0; i<nnz-1; i++) {
      mat[adr+i] *= tmp;
    }

    // update row c<r where mat(r,c)!=0
    for (int i=0; i<nnz-1; i++) {
      // get column index
      int c = colind[adr+i];

      // mat(c,0:c) = mat(c,0:c) - mat(r,c) * mat(r,0:c)
      int nnz_c = mju_combineSparse(mat + rowadr[c], mat+rowadr[r], c + 1, 1, -mat[adr+i],
                                    rownnz[c], i+1, colind+rowadr[c], colind+rowadr[r],
                                    sparse_buf, buf_ind);

      // assign new nnz to row c
      rownnz[c] = nnz_c;
    }
  }

  mjFREESTACK;
  return rank;
}



// sparse reverse-order Cholesky solve
void mju_cholSolveSparse(mjtNum* res, const mjtNum* mat, const mjtNum* vec, int n,
                         const int* rownnz, const int* rowadr, const int* colind) {
  // copy input into result
  mju_copy(res, vec, n);

  // vec <- L^-T vec
  for (int i=n-1; i>=0; i--) {
    if (res[i]) {
      // get rowadr[i], rownnz[i]
      const int adr = rowadr[i], nnz = rownnz[i];

      // x(i) /= L(i,i)
      res[i] /= mat[adr+nnz-1];
      mjtNum tmp = res[i];

      // x(j) -= L(i,j)*x(i), j=0:i-1
      for (int j=0; j<nnz-1; j++) {
        res[colind[adr+j]] -= mat[adr+j]*tmp;
      }
    }
  }

  // vec <- L^-1 vec
  for (int i=0; i<n; i++) {
    // get rowadr[i], rownnz[i]
    const int adr = rowadr[i], nnz = rownnz[i];

    // x(i) -= sum_j L(i,j)*x(j), j=0:i-1
    if (nnz>1) {
      res[i] -= mju_dotSparse(mat+adr, res, nnz-1, colind+adr);
      // modulo AVX, the above line does
      // for (int j=0; j<nnz-1; j++)
      //   res[i] -= mat[adr+j]*res[colind[adr+j]];
    }


    // x(i) /= L(i,i)
    res[i] /= mat[adr+nnz-1];
  }
}



// sparse reverse-order Cholesky rank-one update: L'*L +/- x*x'; return rank
//  x is sparse, change in sparsity pattern of mat is not allowed
int mju_cholUpdateSparse(mjtNum* mat, mjtNum* x, int n, int flg_plus,
                         int* rownnz, int* rowadr, int* colind, int x_nnz, int* x_ind,
                         mjData* d) {
  mjMARKSTACK;
  int* buf_ind = (int*) mj_stackAlloc(d, n);
  mjtNum* sparse_buf = mj_stackAlloc(d, n);

  // backpass over rows corresponding to non-zero x(r)
  int rank = n, i = x_nnz - 1;
  while (i>=0) {
    // get rownnz and rowadr for this row
    int nnz = rownnz[x_ind[i]], adr = rowadr[x_ind[i]];

    // compute quantities
    mjtNum tmp = mat[adr+nnz-1]*mat[adr+nnz-1] + (flg_plus ? x[i]*x[i] : -x[i]*x[i]);
    if (tmp<mjMINVAL) {
      tmp = mjMINVAL;
      rank--;
    }
    mjtNum r = mju_sqrt(tmp);
    mjtNum c = r / mat[adr+nnz-1];
    mjtNum s = x[i] / mat[adr+nnz-1];

    // update diagonal
    mat[adr+nnz-1] = r;

    // update row:  mat(r,1:r-1) = (mat(r,1:r-1) + s*x(1:r-1)) / c
    int new_nnz = mju_combineSparse(mat + adr, x, n, 1 / c, (flg_plus ? s / c : -s / c),
                                    nnz-1, i, colind + adr, x_ind,
                                    sparse_buf, buf_ind);

    // check for size change
    if (new_nnz!=nnz-1) {
      mju_error("Varying sparsity pattern in mju_cholUpdateSparse");
    }

    // update x:  x(1:r-1) = c*x(1:r-1) - s*mat(r,1:r-1)
    int new_x_nnz = mju_combineSparse(x, mat+adr, n, c, -s,
                                      i, nnz-1, x_ind, colind+adr,
                                      sparse_buf, buf_ind);

    // update i, correct for changing x
    i = i - 1 + (new_x_nnz - i);
  }

  mjFREESTACK;
  return rank;
}



//------------------------------ LU factorization --------------------------------------------------

// sparse reverse-order LU factorization, no fill-in (assuming tree topology)
//   result: LU = L + U; original = (U+I) * L; scratch size is n
void mju_factorLUSparse(mjtNum* LU, int n, int* scratch,
                        const int* rownnz, const int* rowadr, const int* colind) {
  int* remaining = scratch;

  // set remaining = rownnz
  memcpy(remaining, rownnz, n*sizeof(int));

  // diagonal elements (i,i)
  for (int i=n-1; i>=0; i--) {
    // get address of last remaining element of row i, adjust remaining counter
    int ii = rowadr[i] + remaining[i] - 1;
    remaining[i]--;

    // make sure ii is on diagonal
    if (colind[ii]!=i) {
      mju_error("missing diagonal element in mju_factorLUSparse");
    }

    // make sure diagonal is not too small
    if (mju_abs(LU[ii])<mjMINVAL) {
      mju_error("diagonal element too small in mju_factorLUSparse");
    }

    // rows j above i
    for (int j=i-1; j>=0; j--) {
      // get address of last remaining element of row j
      int ji = rowadr[j] + remaining[j] - 1;

      // process row j if (j,i) is non-zero
      if (colind[ji]==i) {
        // adjust remaining counter
        remaining[j]--;

        // (j,i) = (j,i) / (i,i)
        LU[ji] = LU[ji] / LU[ii];
        mjtNum LUji = LU[ji];

        // (j,k) = (j,k) - (i,k) * (j,i) for k<i; handle incompatible sparsity
        int icnt = rowadr[i], jcnt = rowadr[j];
        while (jcnt<rowadr[j]+remaining[j]) {
          // both non-zero
          if (colind[icnt]==colind[jcnt]) {
            // update LU, advance counters
            LU[jcnt++] -= LU[icnt++] * LUji;
          }

          // only (j,k) non-zero
          else if (colind[icnt]>colind[jcnt]) {
            // advance j counter
            jcnt++;
          }

          // only (i,k) non-zero
          else {
            mju_error("mju_factorLUSparse requires fill-in");
          }
        }

        // make sure both rows fully processed
        if (icnt!=rowadr[i]+remaining[i] || jcnt!=rowadr[j]+remaining[j]) {
          mju_error("row processing incomplete in mju_factorLUSparse");
        }
      }
    }
  }

  // make sure remaining points to diagonal
  for (int i=0; i<n; i++) {
    if (remaining[i]<0 || colind[rowadr[i]+remaining[i]]!=i) {
      mju_error("unexpected sparse matrix structure in mju_factorLUSparse");
    }
  }
}



// solve mat*res=vec given LU factorization of mat
void mju_solveLUSparse(mjtNum* res, const mjtNum* LU, const mjtNum* vec, int n,
                       const int* rownnz, const int* rowadr, const int* colind) {
  //------------------ solve (U+I)*res = vec
  for (int i=n-1; i>=0; i--) {
    // init: diagonal of (U+I) is 1
    res[i] = vec[i];

    // res[i] -= sum_k>i res[k]*LU(i,k)
    int j = rownnz[i] - 1;
    while (colind[rowadr[i]+j]>i) {
      res[i] -= res[colind[rowadr[i]+j]] * LU[rowadr[i]+j];
      j--;
    }

    // make sure j points to diagonal
    if (colind[rowadr[i]+j]!=i) {
      mju_error("diagonal of U not reached in mju_factorLUSparse");
    }
  }

  //------------------ solve L*res(new) = res
  for (int i=0; i<n; i++) {
    // res[i] -= sum_k<i res[k]*LU(i,k)
    int j = 0;
    while (colind[rowadr[i]+j]<i) {
      res[i] -= res[colind[rowadr[i]+j]] * LU[rowadr[i]+j];
      j++;
    }

    // divide by diagonal element of L
    res[i] /= LU[rowadr[i]+j];

    // make sure j points to diagonal
    if (colind[rowadr[i]+j]!=i) {
      mju_error("diagonal of L not reached in mju_factorLUSparse");
    }
  }
}



//--------------------------- eigen decomposition --------------------------------------------------

// eigenvalue decomposition of symmetric 3x3 matrix
static const mjtNum eigEPS = 1E-12;
int mju_eig3(mjtNum* eigval, mjtNum* eigvec, mjtNum quat[4], const mjtNum mat[9]) {
  mjtNum D[9], tmp[9];
  mjtNum tau, t, c;
  int iter, rk, ck, rotk;

  // initialize with unit quaternion
  quat[0] = 1;
  quat[1] = quat[2] = quat[3] = 0;

  // Jacobi iteration
  for (iter=0; iter<500; iter++) {
    // make quaternion matrix eigvec, compute D = eigvec'*mat*eigvec
    mju_quat2Mat(eigvec, quat);
    mju_mulMatTMat(tmp, eigvec, mat, 3, 3, 3);
    mju_mulMatMat(D, tmp, eigvec, 3, 3, 3);

    // assign eigenvalues
    eigval[0] = D[0];
    eigval[1] = D[4];
    eigval[2] = D[8];

    // find max off-diagonal element, set indices
    if (fabs(D[1])>fabs(D[2]) && fabs(D[1])>fabs(D[5])) {
      rk = 0;     // row
      ck = 1;     // column
      rotk = 2;   // rotation axis
    } else if (fabs(D[2])>fabs(D[5])) {
      rk = 0;
      ck = 2;
      rotk = 1;
    } else {
      rk = 1;
      ck = 2;
      rotk = 0;
    }

    // terminate if max off-diagonal element too small
    if (fabs(D[3*rk+ck])<eigEPS) {
      break;
    }

    // 2x2 symmetric Schur decomposition
    tau = (D[4*ck]-D[4*rk])/(2*D[3*rk+ck]);
    if (tau>=0) {
      t = 1.0/(tau + mju_sqrt(1 + tau*tau));
    } else {
      t = -1.0/(-tau + mju_sqrt(1 + tau*tau));
    }
    c = 1.0/mju_sqrt(1 + t*t);

    // terminate if cosine too close to 1
    if (c>1.0-eigEPS) {
      break;
    }

    // express rotation as quaternion
    tmp[1] = tmp[2] = tmp[3] = 0;
    tmp[rotk+1] = (tau>=0 ? -mju_sqrt(0.5-0.5*c) : mju_sqrt(0.5-0.5*c));
    if (rotk==1) {
      tmp[rotk+1] = -tmp[rotk+1];
    }
    tmp[0] = mju_sqrt(1.0 - tmp[rotk+1]*tmp[rotk+1]);
    mju_normalize4(tmp);

    // accumulate quaternion rotation
    mju_mulQuat(tmp+4, quat, tmp);
    mju_copy4(quat, tmp+4);
    mju_normalize4(quat);
  }

  // sort eigenvalues in decreasing order (bubblesort: 0, 1, 0)
  for (int j=0; j<3; j++) {
    int j1 = j%2;       // lead index

    if (eigval[j1] < eigval[j1+1]) {
      // swap eigenvalues
      t = eigval[j1];
      eigval[j1] = eigval[j1+1];
      eigval[j1+1] = t;

      // rotate quaternion
      tmp[0] = 0.707106781186548;     // mju_cos(pi/4) = mju_sin(pi/4)
      tmp[1] = tmp[2] = tmp[3] = 0;
      tmp[(j1+2)%3+1] = tmp[0];
      mju_mulQuat(tmp+4, quat, tmp);
      mju_copy4(quat, tmp+4);
      mju_normalize4(quat);
    }
  }

  // recompute eigvec
  mju_quat2Mat(eigvec, quat);

  return iter;
}



//---------------------------------- QCQP ----------------------------------------------------------

// solve QCQP in 2 dimensions:
//  min  0.5*x'*A*x + x'*b  s.t.  sum (xi/di)^2 <= r^2
// return 0 if unconstrained, 1 if constrained
int mju_QCQP2(mjtNum* res, const mjtNum* Ain, const mjtNum* bin,
              const mjtNum* d, mjtNum r) {
  mjtNum A11, A22, A12, b1, b2;
  mjtNum P11, P22, P12, det, detinv, v1, v2, la, val, deriv;

  // scale A,b so that constraint becomes x'*x <= r*r
  b1 = bin[0]*d[0];
  b2 = bin[1]*d[1];
  A11 = Ain[0]*d[0]*d[0];
  A22 = Ain[3]*d[1]*d[1];
  A12 = Ain[1]*d[0]*d[1];

  // Newton iteration
  la = 0;
  for (int iter=0; iter<20; iter++) {
    // det(A+la)
    det = (A11+la)*(A22+la) - A12*A12;

    // check SPD, with 1e-10 threshold
    if (det<1e-10) {
      res[0] = 0;
      res[1] = 0;
      return 0;
    }

    // P = inv(A+la)
    detinv = 1/det;
    P11 = (A22+la)*detinv;
    P22 = (A11+la)*detinv;
    P12 = -A12*detinv;

    // v = -P*b
    v1 = -P11*b1 - P12*b2;
    v2 = -P12*b1 - P22*b2;

    // val = v'*v - r*r
    val = v1*v1 + v2*v2 - r*r;

    // check for convergence, or initial solution inside constraint set
    if (val<1e-10) {
      break;
    }

    // deriv = -2 * v' * P * v
    deriv = -2.0*(P11*v1*v1 + 2.0*P12*v1*v2 + P22*v2*v2);

    // compute update, exit if too small
    mjtNum delta = -val/deriv;
    if (delta<1e-10) {
      break;
    }

    // update
    la += delta;
  }

  // undo scaling
  res[0] = v1*d[0];
  res[1] = v2*d[1];

  return (la!=0);
}



// solve QCQP in 3 dimensions:
//  min  0.5*x'*A*x + x'*b  s.t.  sum (xi/di)^2 <= r^2
// return 0 if unconstrained, 1 if constrained
int mju_QCQP3(mjtNum* res, const mjtNum* Ain, const mjtNum* bin,
              const mjtNum* d, mjtNum r) {
  mjtNum A11, A22, A33, A12, A13, A23, b1, b2, b3;
  mjtNum P11, P22, P33, P12, P13, P23, det, detinv, v1, v2, v3, la, val, deriv;

  // scale A,b so that constraint becomes x'*x <= r*r
  b1 = bin[0]*d[0];
  b2 = bin[1]*d[1];
  b3 = bin[2]*d[2];
  A11 = Ain[0]*d[0]*d[0];
  A22 = Ain[4]*d[1]*d[1];
  A33 = Ain[8]*d[2]*d[2];
  A12 = Ain[1]*d[0]*d[1];
  A13 = Ain[2]*d[0]*d[2];
  A23 = Ain[5]*d[1]*d[2];

  // Newton iteration
  la = 0;
  for (int iter=0; iter<20; iter++) {
    // unscaled P
    P11 = (A22+la)*(A33+la) - A23*A23;
    P22 = (A11+la)*(A33+la) - A13*A13;
    P33 = (A11+la)*(A22+la) - A12*A12;
    P12 = A13*A23 - A12*(A33+la);
    P13 = A12*A23 - A13*(A22+la);
    P23 = A12*A13 - A23*(A11+la);

    // det(A+la)
    det = (A11+la)*P11 + A12*P12 + A13*P13;

    // check SPD, with 1e-10 threshold
    if (det<1e-10) {
      res[0] = 0;
      res[1] = 0;
      res[2] = 0;
      return 0;
    }

    // detinv
    detinv = 1/det;

    // final P
    P11 *= detinv;
    P22 *= detinv;
    P33 *= detinv;
    P12 *= detinv;
    P13 *= detinv;
    P23 *= detinv;

    // v = -P*b
    v1 = -P11*b1 - P12*b2 - P13*b3;
    v2 = -P12*b1 - P22*b2 - P23*b3;
    v3 = -P13*b1 - P23*b2 - P33*b3;

    // val = v'*v - r*r
    val = v1*v1 + v2*v2 + v3*v3 - r*r;

    // check for convergence, or initial solution inside constraint set
    if (val<1e-10) {
      break;
    }

    // deriv = -2 * v' * P * v
    deriv = -2.0*(P11*v1*v1 + P22*v2*v2 + P33*v3*v3)
            -4.0*(P12*v1*v2 + P13*v1*v3 + P23*v2*v3);

    // compute update, exit if too small
    mjtNum delta = -val/deriv;
    if (delta<1e-10) {
      break;
    }

    // update
    la += delta;
  }

  // undo scaling
  res[0] = v1*d[0];
  res[1] = v2*d[1];
  res[2] = v3*d[2];

  return (la!=0);
}



// solve QCQP in n dimensions:
//  min  0.5*x'*A*x + x'*b  s.t.  sum (xi/di)^2 <= r^2
// return 0 if unconstrained, 1 if constrained
int mju_QCQP(mjtNum* res, const mjtNum* Ain, const mjtNum* bin,
             const mjtNum* d, mjtNum r, int n) {
  mjtNum A[25], Ala[25], b[5];
  mjtNum la, val, deriv, tmp[5];

  // check size
  if (n>5) {
    mju_error("mju_QCQP supports n up to 5");
  }

  // scale A,b so that constraint becomes x'*x <= r*r
  for (int i=0; i<n; i++) {
    b[i] = bin[i] * d[i];

    for (int j=0; j<n; j++) {
      A[j+i*n] = Ain[j+i*n] * d[i] * d[j];
    }
  }

  // Newton iteration
  la = 0;
  for (int iter=0; iter<20; iter++) {
    // make A+la
    mju_copy(Ala, A, n*n);
    for (int i=0; i<n; i++) {
      Ala[i*(n+1)] += la;
    }

    // factorize, check rank with 1e-10 threshold
    if (mju_cholFactor(Ala, n, 1e-10) < n) {
      mju_zero(res, n);
      return 0;
    }

    // set res = -Ala \ b
    mju_cholSolve(res, Ala, b, n);
    mju_scl(res, res, -1, n);

    // val = b' * Ala^-2 * b - r*r
    val = mju_dot(res, res, n) - r*r;

    // check for convergence, or initial solution inside constraint set
    if (val<1e-10) {
      break;
    }

    // deriv = -2 * b' * Ala^-3 * b
    mju_cholSolve(tmp, Ala, res, n);
    deriv = -2.0 * mju_dot(res, tmp, n);

    // compute update, exit if too small
    mjtNum delta = -val/deriv;
    if (delta<1e-10) {
      break;
    }

    // update
    la += delta;
  }

  // undo scaling
  for (int i=0; i<n; i++) {
    res[i] = res[i] * d[i];
  }

  return (la!=0);
}