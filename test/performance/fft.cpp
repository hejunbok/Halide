// This FFT is an implementation of the algorithm described in
// http://research.microsoft.com/pubs/131400/fftgpusc08.pdf
// This algorithm is more well suited to Halide than in-place
// algorithms.

#include <stdio.h>
#include <Halide.h>
#include <vector>
#include "clock.h"

const float pi = 3.14159265f;

using namespace Halide;

// Find the best radix to use for an FFT of size N. Currently, this is
// always 2.
int find_radix(int N) {
    return 2;
}

// Complex number arithmetic. Complex numbers are represented with
// Halide Tuples.
Expr re(Tuple z) {
    return z[0];
}

Expr im(Tuple z) {
    return z[1];
}

Tuple add(Tuple za, Tuple zb) {
    return Tuple(re(za) + re(zb), im(za) + im(zb));
}

Tuple sub(Tuple za, Tuple zb) {
    return Tuple(re(za) - re(zb), im(za) - im(zb));
}

Tuple mul(Tuple za, Tuple zb) {
    return Tuple(re(za)*re(zb) - im(za)*im(zb), re(za)*im(zb) + re(zb)*im(za));
}

// Scalar multiplication.
Tuple scale(Expr x, Tuple z) {
    return Tuple(x*re(z), x*im(z));
}

Tuple conj(Tuple z) {
    return Tuple(re(z), -im(z));
}

// Compute exp(j*x)
Tuple expj(Expr x) {
    return Tuple(cos(x), sin(x));
}

// Some helpers for doing basic Halide operations with complex numbers.
Tuple sumz(Tuple z, const std::string &s = "sum") {
    return Tuple(sum(re(z), s + "_re"), sum(im(z), s + "_im"));
}

Tuple selectz(Expr c, Tuple t, Tuple f) {
    return Tuple(select(c, re(t), re(f)), select(c, im(t), im(f)));
}

Tuple selectz(Expr c1, Tuple t1, Expr c2, Tuple t2, Expr c3, Tuple t3, Tuple f) {
    return Tuple(select(c1, re(t1), c2, re(t2), c3, re(t3), re(f)),
                 select(c1, im(t1), c2, im(t2), c3, im(t3), im(f)));
}

// Compute the complex DFT of size N on dimension 0 of x.
Func dft_dim0(Func x, int N, float sign) {
    Func dft("dft_dim0");
    Var n("n");
    RDom k(0, N);
    dft(n, _) = sumz(mul(expj((sign*2*pi*k*n)/N), x(k, _)));
    return dft;
}

// Specializations for some small DFTs.
Func dft2_dim0(Func x, float sign) {
    Var n("n");
    Func dft("dft2_dim0");
#if 0
    dft(n, _) = add(Tuple(undef<float>(), undef<float>()), x(n, _));
    dft(0, _) = add(x(0, _), x(1, _));
    dft(1, _) = sub(x(0, _), x(1, _));
#else
    dft(n, _) = selectz(n == 0,
                        add(x(0, _), x(1, _)),
                        sub(x(0, _), x(1, _)));
#endif
    return dft;
}

Func dft4_dim0(Func x, float sign) {
    const int t0 = 4, t1 = 5, t2 = 6, t3 = 7;
    Var n("n");
    Func dft("dft4_dim0");
    dft(n, _) = add(Tuple(undef<float>(), undef<float>()), x(n%4, _));

    // Butterfly stage 1.
    dft(t0, _) = add(x(0, _), x(2, _));
    dft(t2, _) = sub(x(0, _), x(2, _));
    dft(t1, _) = add(x(1, _), x(3, _));
    dft(t3, _) = mul(sub(x(1, _), x(3, _)), Tuple(0.0f, sign)); // W = j*sign

    // Butterfly stage 2.
    dft(0, _) = add(dft(t0, _), dft(t1, _));
    dft(1, _) = add(dft(t2, _), dft(t3, _));
    dft(2, _) = sub(dft(t0, _), dft(t1, _));
    dft(3, _) = sub(dft(t2, _), dft(t3, _));

    return dft;

/*
    Var nx("nx"), ny("ny");
    Func s("dft4_dim0_s");
    s(nx, ny, _) = x(ny*2 + nx, _);

    Func s1("dft4_dim0_1");
    Tuple W3 = selectz(nx == 0, Tuple(1.0f, 0.0f), Tuple(0.0f, sign));
    s1(nx, ny, _) = selectz(ny == 0,
                            add(s(nx, 0, _), s(nx, 1, _)),
                            mul(sub(s(nx, 0, _), s(nx, 1, _)), W3));
    // The twiddle factor.
    //s1(1, 1, _) = mul(s1(1, 1, _), Tuple(0.0f, sign));

    Var n("n");
    Func s2("dft4_dim0_2");
    Expr nx_ = n/2;
    Expr ny_ = n%2;
    s2(n, _) = selectz(nx_ == 0,
                       add(s1(0, ny_, _), s1(1, ny_, _)),
                       sub(s1(0, ny_, _), s1(1, ny_, _)));

    s1.compute_at(at, atv);
    s1.bound(ny, 0, 2);
    s1.bound(_0, 0, 16);

    return s2;
*/
}

Func dft8_dim0(Func x, float sign) {
    return dft_dim0(x, 8, sign);
}

std::map<int, Func> twiddles;

// Return a function computing the twiddle factors.
Func W(int N, float sign) {
    // Check to see if this set of twiddle factors is already computed.
    Func &w = twiddles[N*(int)sign];

    Var n("n");
    if (!w.defined()) {
        Func W("W");
        // If N is small, we inline the twiddle factors because they
        // contain a lot of zeros -> they simplify a lot.
        switch (N) {
        case 2:
            // N = 2 -> n = 0.
            w(n) = Tuple(1.0f, 0.0f);
            break;
        case 4:
            w(n) = selectz(n%4 == 0, Tuple( 1.0f,  0.0f),
                           n%4 == 1, Tuple( 0.0f,  sign),
                           n%4 == 2, Tuple(-1.0f,  0.0f),
                                     Tuple( 0.0f, -sign));
            break;
        default:
            W(n) = expj((sign*2*pi*n)/N);
            Realization compute_static = W.realize(N);
            Image<float> reW = compute_static[0];
            Image<float> imW = compute_static[1];
            w(n) = Tuple(reW(n), imW(n));
            break;
        }
    }

    return w;
}

// Compute the N point DFT of dimension 1 (columns) of x using
// radix R.
Func fft_dim1(Func x, int N, int R, float sign) {
    Var n0("n0"), n1("n1");

    std::vector<Func> stages;

    RDom rs(0, R, 0, N/R);
    RVar r_ = rs.x;
    RVar s_ = rs.y;
    for (int S = 1; S < N; S *= R) {
        std::stringstream stage_id;
        stage_id << "S" << S << "_R" << R;

        Func exchange("exchange_" + stage_id.str());
        Var r("r"), s("s");

        // Twiddle factors.
        Func w = W(R*S, sign);

        // Load the points from each subtransform and apply the
        // twiddle factors.
        Func v("v_" + stage_id.str());
        v(r, s, n0, _) = mul(w(r*(s%S)), x(n0, s + r*(N/R), _));

        // Compute the R point DFT of the subtransform.
        Func V;
        switch (R) {
        case 2: V = dft2_dim0(v, sign); break;
        case 4: V = dft4_dim0(v, sign); break;
        case 8: V = dft8_dim0(v, sign); break;
        default: V = dft_dim0(v, R, sign); break;
        }

        // Write the subtransform and use it as input to the next
        // pass.
        exchange(n0, n1, _) = add(Tuple(undef<float>(), undef<float>()), x(n0, n1, _));
        exchange(n0, (s_/S)*R*S + s_%S + r_*S, _) = V(r_, s_, n0, _);
        exchange.bound(n1, 0, N);

        // Remember this stage for scheduling later.
        stages.push_back(exchange);

        x = exchange;
    }

    // Split the tile into groups of DFTs, and vectorize within the
    // group.
    Var n0o;
    x.compute_root().update().split(n0, n0o, n0, 8).reorder(n0, r_, s_, n0o).vectorize(n0);
    for (size_t i = 0; i < stages.size() - 1; i++) {
        //stages[i].compute_at(x, n0o).update().vectorize(n0);
        stages[i].compute_root().update().vectorize(n0, 8);
    }
    return x;
}

// Transpose the first two dimensions of x.
Func transpose(Func x) {
    std::vector<Var> argsT(x.args());
    std::swap(argsT[0], argsT[1]);
    Func xT;
    xT(argsT) = x(x.args());
    return xT;
}

// Compute the N0 x N1 2D complex DFT of x using radixes R0, R1.
// sign = -1 indicates a forward DFT, sign = 1 indicates an inverse
// DFT.
Func fft2d_c2c(Func x, int N0, int R0, int N1, int R1, float sign) {
    // Transpose the input to the FFT.
    Func xT = transpose(x);

    // Compute the DFT of dimension 1 (originally dimension 0).
    Func dft1T = fft_dim1(xT, N0, R0, sign);

    // Transpose back.
    Func dft1 = transpose(dft1T);

    // Compute the DFT of dimension 1.
    Func dft = fft_dim1(dft1, N1, R1, sign);
    dft.bound(dft.args()[0], 0, N0);
    dft.bound(dft.args()[1], 0, N1);
    return dft;
}

// Compute the N0 x N1 2D complex DFT of x. sign = -1 indicates a
// forward DFT, sign = 1 indicates an inverse DFT.
Func fft2d_c2c(Func c, int N0, int N1, float sign) {
    return fft2d_c2c(c, N0, find_radix(N0), N1, find_radix(N1), sign);
}

// Compute the N0 x N1 2D real DFT of x using radixes R0, R1.
// Note that the transform domain is transposed and has dimensions
// N1/2+1 x N0 due to the conjugate symmetry of real DFTs.
Func fft2d_r2cT(Func r, int N0, int R0, int N1, int R1) {
    Var n0("n0"), n1("n1");

    // Combine pairs of real columns x, y into complex columns
    // z = x + j*y. This allows us to compute two real DFTs using
    // one complex FFT.

    // Grab columns from each half of the input data to improve
    // coherency of the zip/unzip operations, which improves
    // vectorization.
    Func zipped("zipped");
    zipped(n0, n1, _) = Tuple(r(n0, n1, _),
                              r(n0 + N0/2, n1, _));

    // DFT down the columns first.
    Func dft1 = fft_dim1(zipped, N1, R1, -1);

    // Unzip the DFTs of the columns.
    Func unzipped("unzipped");
    // By linearity of the DFT, Z = X + j*Y, where X, Y, and Z are the
    // DFTs of x, y and z.

    // By the conjugate symmetry of real DFTs, computing Z_n +
    // conj(Z_(N-n)) and Z_n - conj(Z_(N-n)) gives 2*X_n and 2*j*Y_n,
    // respectively.
    Tuple Z = dft1(n0%(N0/2), n1, _);
    Tuple symZ = dft1(n0%(N0/2), (N1 - n1)%N1, _);
    Tuple X = scale(0.5f, add(Z, conj(symZ)));
    Tuple Y = mul(Tuple(0.0f, -0.5f), sub(Z, conj(symZ)));
    unzipped(n0, n1, _) = selectz(n0 < N0/2, X, Y);
    unzipped.compute_root().vectorize(n0, 8).unroll(n0);

    // Transpose so we can FFT dimension 0 (by making it dimension 1).
    Func unzippedT = transpose(unzipped);

    // DFT down the columns again (the rows of the original).
    Func dft = fft_dim1(unzippedT, N0, R0, -1);
    dft.bound(dft.args()[0], 0, N1/2 + 1);
    dft.bound(dft.args()[1], 0, N0);

    Func dft_clamped;
    dft_clamped(n1, n0, _) = dft(clamp(n1, 0, N1/2), n0, _);
    return dft_clamped;
}

// Compute the N0 x N1 2D inverse DFT of x using radixes R0, R1.
// Note that the input domain is transposed and should have dimensions
// N1/2+1 x N0 due to the conjugate symmetry of real FFTs.
Func fft2d_cT2r(Func cT, int N0, int R0, int N1, int R1) {
    Var n0("n0"), n1("n1");

    Func cT_clamped;
    cT_clamped(n1, n0, _) = cT(clamp(n1, 0, N1/2), n0, _);

    // Take the inverse DFT of the columns (rows in the final result).
    Func dft1T = fft_dim1(cT_clamped, N0, R0, 1);

    // Transpose so we can take the DFT of the columns again.
    Func dft1 = transpose(dft1T);

    // Zip two real DFTs X and Y into one complex DFT Z = X + j*Y
    Func zipped("zipped");
    // Construct the whole DFT domain of X and Y via conjugate
    // symmetry.
    Tuple X = selectz(n1 < N1/2 + 1,
                      dft1(n0, clamp(n1, 0, N1/2), _),
                      conj(dft1(n0, clamp((N1 - n1)%N1, 0, N1/2), _)));
    Tuple Y = selectz(n1 < N1/2 + 1,
                      dft1(n0 + N0/2, clamp(n1, 0, N1/2), _),
                      conj(dft1(n0 + N0/2, clamp((N1 - n1)%N1, 0, N1/2), _)));
    zipped(n0, n1, _) = add(X, mul(Tuple(0.0f, 1.0f), Y));
    zipped.compute_root().vectorize(n0, 8);

    // Take the inverse DFT of the columns again.
    Func dft = fft_dim1(zipped, N1, R1, 1);

    // Extract the real inverse DFTs.
    Func unzipped("unzipped");
    unzipped(n0, n1, _) = select(n0 < N0/2,
                                 re(dft(n0%(N0/2), n1, _)),
                                 im(dft(n0%(N0/2), n1, _)));
    unzipped.compute_root().vectorize(n0, 8).unroll(n0);
    unzipped.bound(n0, 0, N0);
    unzipped.bound(n1, 0, N1);
    return unzipped;
}

// Compute N0 x N1 real DFTs.
Func fft2d_r2cT(Func r, int N0, int N1) {
    return fft2d_r2cT(r, N0, find_radix(N0), N1, find_radix(N1));
}
Func fft2d_cT2r(Func cT, int N0, int N1) {
    return fft2d_cT2r(cT, N0, find_radix(N0), N1, find_radix(N1));
}


template <typename T>
Func make_real(Image<T> &img) {
    Var x, y, z;
    Func ret;
    ret(x, y) = img(x, y);
    return ret;
}

template <typename T>
Func make_complex(Image<T> &img) {
    Var x, y, z;
    Func ret;
    ret(x, y) = Tuple(img(x, y), 0.0f);
    return ret;
}

double log2(double x) {
    return log(x)/log(2.0);
}

int main(int argc, char **argv) {

    // Generate a random image to convolve with.
    const int W = 64, H = 64;

    Image<float> in(W, H);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            in(x, y) = (float)rand()/(float)RAND_MAX;
        }
    }

    // Construct a box filter kernel centered on the origin.
    const int box = 3;
    Image<float> kernel(W, H);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int u = x < (W - x) ? x : (W - x);
            int v = y < (H - y) ? y : (H - y);
            kernel(x, y) = u <= box/2 && v <= box/2 ? 1.0f/(box*box) : 0.0f;
        }
    }

    Target target = get_jit_target_from_environment();

    Var x("x"), y("y");

    Func filtered_c2c;
    {
        // Compute the DFT of the input and the kernel.
        Func dft_in = fft2d_c2c(make_complex(in), W, H, -1);
        Func dft_kernel = fft2d_c2c(make_complex(kernel), W, H, -1);

        // Compute the convolution.
        Func dft_filtered("dft_filtered");
        dft_filtered(x, y) = mul(dft_in(x, y), dft_kernel(x, y));

        // Compute the inverse DFT to get the result.
        Func dft_out = fft2d_c2c(dft_filtered, W, H, 1);

        // Extract the real component and normalize.
        filtered_c2c(x, y) = re(dft_out(x, y))/cast<float>(W*H);
    }

    Func filtered_r2c;
    {
        // Compute the DFT of the input and the kernel.
        Func dft_in = fft2d_r2cT(make_real(in), W, H);
        Func dft_kernel = fft2d_r2cT(make_real(kernel), W, H);

        // Compute the convolution.
        Func dft_filtered("dft_filtered");
        dft_filtered(x, y) = mul(dft_in(x, y), dft_kernel(x, y));
        //dft_filtered.compute_root().trace_realizations();

        // Compute the inverse DFT to get the result.
        filtered_r2c = fft2d_cT2r(dft_filtered, W, H);

        // Normalize the result.
        RDom xy(0, W, 0, H);
        filtered_r2c(xy.x, xy.y) /= cast<float>(W*H);
    }

    Image<float> result_c2c = filtered_c2c.realize(W, H, target);
    Image<float> result_r2c = filtered_r2c.realize(W, H, target);

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float correct = 0;
            for (int i = -box/2; i <= box/2; i++) {
                for (int j = -box/2; j <= box/2; j++) {
                    correct += in((x + j + W)%W, (y + i + H)%H);
                }
            }
            correct /= box*box;
            if (fabs(result_c2c(x, y) - correct) > 1e-6f) {
                printf("result_c2c(%d, %d) = %f instead of %f\n", x, y, result_c2c(x, y), correct);
                return -1;
            }
            if (fabs(result_r2c(x, y) - correct) > 1e-6f) {
                printf("result_r2c(%d, %d) = %f instead of %f\n", x, y, result_r2c(x, y), correct);
                return -1;
            }
        }
    }

    // For a description of the methodology used here, see
    // http://www.fftw.org/speed/method.html

    // Take the minimum time over many of iterations to minimize
    // noise.
    const int samples = 10;
    const int reps = 100;
    double t = 1e6f;

    Func bench_c2c = fft2d_c2c(make_complex(in), W, H, -1);
    Realization R_c2c = bench_c2c.realize(W, H, target);

    for (int i = 0; i < samples; i++) {
        double t1 = current_time();
        for (int j = 0; j < reps; j++) {
            bench_c2c.realize(R_c2c, target);
        }
        double dt = (current_time() - t1)/reps;
        if (dt < t) t = dt;
    }
    printf("c2c  time: %f ms, %f MFLOP/s\n", t, 5*W*H*(log2(W) + log2(H))/t*1e3*1e-6);

    Func bench_r2cT = fft2d_r2cT(make_real(in), W, H);
    Realization R_r2cT = bench_r2cT.realize(H/2 + 1, W, target);

    t = 1e6f;
    for (int i = 0; i < samples; i++) {
        double t1 = current_time();
        for (int j = 0; j < reps; j++) {
            bench_r2cT.realize(R_r2cT, target);
        }
        double dt = (current_time() - t1)/reps;
        if (dt < t) t = dt;
    }
    printf("r2cT time: %f ms, %f MFLOP/s\n", t, 2.5*W*H*(log2(W) + log2(H))/t*1e3*1e-6);

    Image<float> cT(H/2 + 1, W);
    memset(cT.data(), 0, cT.width()*cT.height()*sizeof(float));
    Func bench_cT2r = fft2d_cT2r(make_complex(in), W, H);
    Realization R_cT2r = bench_cT2r.realize(W, H, target);

    t = 1e6f;
    for (int i = 0; i < samples; i++) {
        double t1 = current_time();
        for (int j = 0; j < reps; j++) {
            bench_cT2r.realize(R_cT2r, target);
        }
        double dt = (current_time() - t1)/reps;
        if (dt < t) t = dt;
    }
    printf("cT2r time: %f ms, %f MFLOP/s\n", t, 2.5*W*H*(log2(W) + log2(H))/t*1e3*1e-6);

    twiddles.clear();

    return 0;
}
