  /* This machine-generated function computes a quartet of |0p) first derivative ERIs */

void d1hrr3_build_0p(const double *CD, double *vp, const double *I0, const double *I1,
        double c2, const double *I2, double c3, const double *I3, double c4, const double *I4,
        double c5, const double *I5, double c6, const double *I6, double c7, const double *I7, int ab_num)
{
  int ab;
  const double CD0 = CD[0];
  const double CD1 = CD[1];
  const double CD2 = CD[2];
  for(ab=0;ab<ab_num;ab++) {
    *(vp++) = I0[0] + CD0*I1[0] + c2*I2[0] - c5*I5[0];
    *(vp++) = I0[1] + CD1*I1[0] + c3*I3[0] - c6*I6[0];
    *(vp++) = I0[2] + CD2*I1[0] + c4*I4[0] - c7*I7[0];
    I0 += 3;
    I1 += 1;
    I2 += 1;
    I3 += 1;
    I4 += 1;
    I5 += 1;
    I6 += 1;
    I7 += 1;
  }
}