#include <cmath>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <vector>
#include <string>
#include <cstdio>
#include <filesystem>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

constexpr int WIDTH = 1280;
constexpr int HEIGHT = 720;
constexpr double PI = 3.1415926538;

// Global space background texture
unsigned char* spaceImage = nullptr;
int spaceWidth = 0, spaceHeight = 0, spaceChannels = 0;

// Settings
double camR = 30.0;
double tilt = 0.05;  // Very small tilt - almost edge-on view (Interstellar!)
double tiltAmp = 0.5;  // orbit swoop: elevation rises and falls once per lap (0 = flat edge-on orbit)
double zoom = 1.0;
double a = 0.6;
double discMin = 3.83;  // ISCO
double discMax = 15.0;
double discHeight = 0.03;  // THIN disk for Interstellar-like razor edge (was 0.15)
double eps = 0.01;
double dtau = 0.1;
int maxSteps = 500;

// Motion blur / time integration
int TIME_SAMPLES = 4;  // still: 4 (motion blur). orbit video: set to 1 — blur comes free from frame-to-frame motion, and it's 4x faster
constexpr double DISK_ROTATION_SPEED = 0.3;  // Angular velocity for motion blur

struct Vec3 {
    double x, y, z;
    Vec3 operator+(const Vec3& b) const { return {x+b.x, y+b.y, z+b.z}; }
    Vec3 operator-(const Vec3& b) const { return {x-b.x, y-b.y, z-b.z}; }
    Vec3 operator*(double s) const { return {x*s, y*s, z*s}; }
    Vec3& operator+=(const Vec3& b) { x+=b.x; y+=b.y; z+=b.z; return *this; }
};

struct Vec4 {
    double t, x, y, z;
    Vec4 operator+(const Vec4& b) const { return {t+b.t, x+b.x, y+b.y, z+b.z}; }
    Vec4 operator-(const Vec4& b) const { return {t-b.t, x-b.x, y-b.y, z-b.z}; }
    Vec4 operator*(double s) const { return {t*s, x*s, y*s, z*s}; }
    Vec4 operator/(double s) const { return {t/s, x/s, y/s, z/s}; }
    Vec4& operator+=(const Vec4& b) { t+=b.t; x+=b.x; y+=b.y; z+=b.z; return *this; }
    Vec4& operator-=(const Vec4& b) { t-=b.t; x-=b.x; y-=b.y; z-=b.z; return *this; }
};

struct Mat4 {
    double m[4][4];
};

double dot(const Vec3& a, const Vec3& b) {
    return a.x*b.x + a.y*b.y + a.z*b.z;
}

double dot4(const Vec4& a, const Vec4& b) {
    return a.t*b.t + a.x*b.x + a.y*b.y + a.z*b.z;
}

Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x};
}

double length(const Vec3& v) {
    return std::sqrt(dot(v, v));
}

Vec3 normalize(const Vec3& v) {
    double len = length(v);
    return v * (1.0 / len);
}

// Rotate a vector about the spin axis (z). At ang=0 this is an exact identity
// (cos=1, sin=0), so a still render stays bit-identical to before orbiting existed.
Vec3 rotZ(const Vec3& p, double ang) {
    double c = std::cos(ang), s = std::sin(ang);
    return {p.x*c - p.y*s, p.x*s + p.y*c, p.z};
}

Mat4 diag(const Vec4& v) {
    Mat4 result = {};
    result.m[0][0] = v.t;
    result.m[1][1] = v.x;
    result.m[2][2] = v.y;
    result.m[3][3] = v.z;
    return result;
}

Mat4 outerProduct(const Vec4& a, const Vec4& b) {
    Mat4 result;
    for(int i=0; i<4; i++)
        for(int j=0; j<4; j++)
            result.m[i][j] = (&a.t)[i] * (&b.t)[j];
    return result;
}

Mat4 operator+(const Mat4& a, const Mat4& b) {
    Mat4 result;
    for(int i=0; i<4; i++)
        for(int j=0; j<4; j++)
            result.m[i][j] = a.m[i][j] + b.m[i][j];
    return result;
}

Mat4 operator*(const Mat4& a, double s) {
    Mat4 result;
    for(int i=0; i<4; i++)
        for(int j=0; j<4; j++)
            result.m[i][j] = a.m[i][j] * s;
    return result;
}

Vec4 operator*(const Mat4& m, const Vec4& v) {
    return {
        m.m[0][0]*v.t + m.m[0][1]*v.x + m.m[0][2]*v.y + m.m[0][3]*v.z,
        m.m[1][0]*v.t + m.m[1][1]*v.x + m.m[1][2]*v.y + m.m[1][3]*v.z,
        m.m[2][0]*v.t + m.m[2][1]*v.x + m.m[2][2]*v.y + m.m[2][3]*v.z,
        m.m[3][0]*v.t + m.m[3][1]*v.x + m.m[3][2]*v.y + m.m[3][3]*v.z
    };
}

Mat4 inverse(const Mat4& A) {
    double m[16], invOut[16];
    for(int i=0; i<4; i++)
        for(int j=0; j<4; j++)
            m[i*4+j] = A.m[i][j];
    
    invOut[0] = m[5]*m[10]*m[15]-m[5]*m[11]*m[14]-m[9]*m[6]*m[15]+m[9]*m[7]*m[14]+m[13]*m[6]*m[11]-m[13]*m[7]*m[10];
    invOut[4] = -m[4]*m[10]*m[15]+m[4]*m[11]*m[14]+m[8]*m[6]*m[15]-m[8]*m[7]*m[14]-m[12]*m[6]*m[11]+m[12]*m[7]*m[10];
    invOut[8] = m[4]*m[9]*m[15]-m[4]*m[11]*m[13]-m[8]*m[5]*m[15]+m[8]*m[7]*m[13]+m[12]*m[5]*m[11]-m[12]*m[7]*m[9];
    invOut[12] = -m[4]*m[9]*m[14]+m[4]*m[10]*m[13]+m[8]*m[5]*m[14]-m[8]*m[6]*m[13]-m[12]*m[5]*m[10]+m[12]*m[6]*m[9];
    invOut[1] = -m[1]*m[10]*m[15]+m[1]*m[11]*m[14]+m[9]*m[2]*m[15]-m[9]*m[3]*m[14]-m[13]*m[2]*m[11]+m[13]*m[3]*m[10];
    invOut[5] = m[0]*m[10]*m[15]-m[0]*m[11]*m[14]-m[8]*m[2]*m[15]+m[8]*m[3]*m[14]+m[12]*m[2]*m[11]-m[12]*m[3]*m[10];
    invOut[9] = -m[0]*m[9]*m[15]+m[0]*m[11]*m[13]+m[8]*m[1]*m[15]-m[8]*m[3]*m[13]-m[12]*m[1]*m[11]+m[12]*m[3]*m[9];
    invOut[13] = m[0]*m[9]*m[14]-m[0]*m[10]*m[13]-m[8]*m[1]*m[14]+m[8]*m[2]*m[13]+m[12]*m[1]*m[10]-m[12]*m[2]*m[9];
    invOut[2] = m[1]*m[6]*m[15]-m[1]*m[7]*m[14]-m[5]*m[2]*m[15]+m[5]*m[3]*m[14]+m[13]*m[2]*m[7]-m[13]*m[3]*m[6];
    invOut[6] = -m[0]*m[6]*m[15]+m[0]*m[7]*m[14]+m[4]*m[2]*m[15]-m[4]*m[3]*m[14]-m[12]*m[2]*m[7]+m[12]*m[3]*m[6];
    invOut[10] = m[0]*m[5]*m[15]-m[0]*m[7]*m[13]-m[4]*m[1]*m[15]+m[4]*m[3]*m[13]+m[12]*m[1]*m[7]-m[12]*m[3]*m[5];
    invOut[14] = -m[0]*m[5]*m[14]+m[0]*m[6]*m[13]+m[4]*m[1]*m[14]-m[4]*m[2]*m[13]-m[12]*m[1]*m[6]+m[12]*m[2]*m[5];
    invOut[3] = -m[1]*m[6]*m[11]+m[1]*m[7]*m[10]+m[5]*m[2]*m[11]-m[5]*m[3]*m[10]-m[9]*m[2]*m[7]+m[9]*m[3]*m[6];
    invOut[7] = m[0]*m[6]*m[11]-m[0]*m[7]*m[10]-m[4]*m[2]*m[11]+m[4]*m[3]*m[10]+m[8]*m[2]*m[7]-m[8]*m[3]*m[6];
    invOut[11] = -m[0]*m[5]*m[11]+m[0]*m[7]*m[9]+m[4]*m[1]*m[11]-m[4]*m[3]*m[9]-m[8]*m[1]*m[7]+m[8]*m[3]*m[5];
    invOut[15] = m[0]*m[5]*m[10]-m[0]*m[6]*m[9]-m[4]*m[1]*m[10]+m[4]*m[2]*m[9]+m[8]*m[1]*m[6]-m[8]*m[2]*m[5];
    
    double invDet = m[0]*invOut[0] + m[1]*invOut[4] + m[2]*invOut[8] + m[3]*invOut[12];
    invDet = 1.0 / invDet;
    
    Mat4 inv;
    for(int i=0; i<4; i++)
        for(int j=0; j<4; j++)
            inv.m[i][j] = invOut[i*4+j] * invDet;
    return inv;
}

double rFromCoords(const Vec4& pos) {
    Vec3 p = {pos.x, pos.y, pos.z};
    double rho2 = dot(p, p) - a*a;
    double r2 = 0.5 * (rho2 + std::sqrt(rho2*rho2 + 4.0*a*a*pos.z*pos.z));
    return std::sqrt(r2);
}

Mat4 metric(const Vec4& pos) {
    double r = rFromCoords(pos);
    Vec4 k = {-1.0, (r*pos.x - a*pos.y)/(r*r+a*a), (r*pos.y + a*pos.x)/(r*r+a*a), pos.z/r};
    double f = 2.0*r / (r*r + a*a*pos.z*pos.z/(r*r));
    return outerProduct(k, k) * f + diag({-1, 1, 1, 1});
}

double hamiltonian(const Vec4& x, const Vec4& p) {
    return 0.5 * dot4(inverse(metric(x)) * p, p);
}

Vec4 hamiltonianGradient(const Vec4& x, const Vec4& p) {
    double h0 = hamiltonian(x, p);
    return Vec4{
        hamiltonian(x + Vec4{eps,0,0,0}, p),
        hamiltonian(x + Vec4{0,eps,0,0}, p),
        hamiltonian(x + Vec4{0,0,eps,0}, p),
        hamiltonian(x + Vec4{0,0,0,eps}, p)
    } * (1.0/eps) - Vec4{h0/eps, h0/eps, h0/eps, h0/eps};
}

void transportStep(Vec4& x, Vec4& p) {
    p -= hamiltonianGradient(x, p) * dtau;
    x += inverse(metric(x)) * p * dtau;
}

bool stopCondition(const Vec4& pos) {
    double r = rFromCoords(pos);
    return r < 1.0 + std::sqrt(1.0 - a*a) || r > std::max(2.0*camR, 30.0);
}

// Realistic space background from texture
Vec3 skyTexture(const Vec3& dir) {
    if (!spaceImage) {
        // Fallback to checkerboard if image failed to load
        double u = 0.5 + std::atan2(dir.y, dir.x) / (2.0*PI);
        double v = 0.5 - std::asin(dir.z) / PI;
        int check = (int(std::floor(u*20)) ^ int(std::floor(v*10))) & 1;
        return check ? Vec3{0.9,0.9,0.9} : Vec3{0.1,0.1,0.1};
    }
    
    // Convert 3D direction to spherical coordinates
    double u = 0.5 + std::atan2(dir.y, dir.x) / (2.0 * PI);
    double v = 0.5 - std::asin(std::fmax(-1.0, std::fmin(1.0, dir.z))) / PI;
    
    // Sample texture
    int px = int(u * spaceWidth) % spaceWidth;
    int py = int(v * spaceHeight) % spaceHeight;
    
    if (px < 0) px += spaceWidth;
    if (py < 0) py += spaceHeight;
    
    int idx = (py * spaceWidth + px) * spaceChannels;
    
    return Vec3{
        spaceImage[idx + 0] / 255.0,
        spaceImage[idx + 1] / 255.0,
        spaceImage[idx + 2] / 255.0
    };
}

// Planck spectrum -> sRGB hue (Tanner Helland approximation), T in Kelvin.
Vec3 blackbodyRGB(double T) {
    T = std::fmax(1000.0, std::fmin(40000.0, T)) / 100.0;
    double r, g, b;
    r = (T <= 66.0) ? 255.0 : 329.698727446 * std::pow(T - 60.0, -0.1332047592);
    g = (T <= 66.0) ? 99.4708025861 * std::log(T) - 161.1195681661
                    : 288.1221695283 * std::pow(T - 60.0, -0.0755148492);
    if (T >= 66.0)      b = 255.0;
    else if (T <= 19.0) b = 0.0;
    else                b = 138.5177312231 * std::log(T - 10.0) - 305.0447927307;
    auto cl = [](double v){ return std::fmax(0.0, std::fmin(255.0, v)) / 255.0; };
    return {cl(r), cl(g), cl(b)};
}

// Optically thin accretion disk emission model
// Returns emitted light contribution (additive, not opaque)
Vec3 diskEmission(const Vec4& pos, const Vec4& p, double timeOffset) {
    double r = rFromCoords(pos);
    
    // Check if in disk radial bounds
    if (r < discMin || r > discMax) return {0, 0, 0};
    
    // Check if within disk height (THICK for visibility)
    double diskH = discHeight * r;
    if (std::abs(pos.z) > diskH) return {0, 0, 0};
    
    // Temperature falloff: hotter near ISCO, cooler at outer edge
    double temp = std::pow((discMax - r) / (discMax - discMin), 1.5);
    
    // Azimuthal angle (with rotation for motion blur)
    double phi = std::atan2(pos.y, pos.x) + timeOffset * DISK_ROTATION_SPEED / std::sqrt(r);
    
    // Azimuthal density modulation - SOFTENED (less banding)
    double spiralPattern = 0.8 + 0.2 * std::sin(phi * 5.0 + r * 2.0);  // Softer spirals
    double ringPattern = 0.9 + 0.1 * std::sin(r * 15.0);  // Softer rings
    double density = spiralPattern * ringPattern;
    
    // Relativistic Doppler factor (clamped to prevent collapse)
    Vec4 discVel = {r + a/std::sqrt(r), 
                   -pos.y * (a >= 0 ? 1 : -1) / std::sqrt(r),
                   pos.x * (a >= 0 ? 1 : -1) / std::sqrt(r), 
                   0.0};
    discVel = discVel * (1.0 / std::sqrt(r*r - 3.0*r + 2.0*a*std::sqrt(r)));
    double doppler = 1.0 / std::abs(dot4(p, discVel));
    doppler = std::max(doppler, 0.1);  // Prevent numerical collapse
    
    // AGGRESSIVE Doppler boost (approaching side burns WHITE)
    double dopplerBoost = std::pow(doppler, 8.0);
    
    // Vertical falloff (Gaussian-like, strongest at midplane)
    double verticalFalloff = std::exp(-std::abs(pos.z) / (diskH * 0.3));
    
    // Intensity with compression to preserve structure
    double intensity = 50.0 * temp * density * std::max(dopplerBoost, 0.5) * verticalFalloff;
    intensity = intensity / (1.0 + intensity);  // Compress extreme values
    
    // Physical color: observed temperature folds in radial falloff AND Doppler.
    // Base runs cool->hot (deep orange outer -> white inner); Doppler^1.5 pushes
    // the approaching side hotter/blue and the receding side cooler/red.
    double Tobs = (1600.0 + 5000.0 * temp) * std::pow(doppler, 1.5);
    Vec3 emissionColor = blackbodyRGB(Tobs) * (0.4 + 0.6 * temp);

    return emissionColor * intensity;
}

Vec3 mainImage(int px, int py, double iTime) {
    double u = (2.0*px - WIDTH) / (double)WIDTH;
    double v = (2.0*py - HEIGHT) / (double)WIDTH;
    
    double az = iTime * 2.0 * PI;  // iTime is orbit progress in [0,1): one full 360
    double tl = tilt + tiltAmp * (1.0 - std::cos(az)) * 0.5;  // swoop: edge-on -> up -> edge-on

    double x = std::sqrt(camR*camR + a*a) * std::cos(tl);
    double z = camR * std::sin(tl);

    Vec4 camPos = {0.0, x, 0.0, z};

    // Camera aim direction - point towards black hole at origin
    Vec3 toCenter = normalize(Vec3{-x, 0.0, -z});

    // Build camera basis
    Vec3 camRight = normalize(Vec3{0.0, 1.0, 0.0});  // y-axis
    Vec3 camUp = normalize(cross(camRight, toCenter));
    camRight = cross(toCenter, camUp);  // Re-orthogonalize

    // Orbit the whole camera rig about the spin axis. Rigid rotation keeps the
    // spin axis vertical in frame. az=0 is an exact identity -> unchanged stills.
    if (az != 0.0) {
        Vec3 cp = rotZ(Vec3{camPos.x, camPos.y, camPos.z}, az);
        camPos.x = cp.x; camPos.y = cp.y; camPos.z = cp.z;
        toCenter = rotZ(toCenter, az);
        camRight = rotZ(camRight, az);
        camUp = rotZ(camUp, az);
    }
    
    // Ray direction in camera space
    Vec3 dir = normalize(toCenter * zoom + camRight * u + camUp * v);
    Vec4 dir4D = {-1.0, dir.x, dir.y, dir.z};
    
    // Accumulate color from multiple time samples (motion blur)
    Vec3 accumulatedColor = {0, 0, 0};
    
    for(int timeSample = 0; timeSample < TIME_SAMPLES; timeSample++) {
        double timeOffset = (timeSample / (double)TIME_SAMPLES) * 2.0 * PI;
        
        Vec4 pos = camPos;
        Vec4 p = metric(pos) * dir4D;
        
        Vec3 diskLight = {0, 0, 0};
        bool captured = false;
        
        for(int i=0; i<maxSteps; i++) {
            // Accumulate disk emission along the ray (volumetric)
            Vec3 emission = diskEmission(pos, p, timeOffset);
            diskLight += emission * dtau * 15.0;  // Boosted to 15x (compensate for thinner disk)
            
            // Check if captured by black hole
            if(stopCondition(pos)) {
                double r = rFromCoords(pos);
                captured = r < 1.0 + std::sqrt(1.0 - a*a);
                break;
            }
            
            transportStep(pos, p);
        }
        
        // Add background (nearly invisible) or disk only
        if(!captured) {
            // Almost invisible space background
            Vec4 dir4D_out = inverse(metric(pos)) * p;
            Vec3 cubeVec = {-dir4D_out.x, dir4D_out.z, -dir4D_out.y};
            accumulatedColor += skyTexture(cubeVec) * 0.05 + diskLight;  // Sky nearly killed
        } else {
            accumulatedColor += diskLight;  // Captured rays only show disk
        }
    }
    
    // Average over time samples
    accumulatedColor = accumulatedColor * (1.0 / TIME_SAMPLES);
    
    // FILMIC tone mapping (exponential, preserves detail)
    double exposure = 0.3;
    Vec3 exposed = accumulatedColor * exposure;
    
    // Exponential tone map (better than hard clamp or Reinhard)
    Vec3 toneMapped = {
        1.0 - std::exp(-exposed.x),
        1.0 - std::exp(-exposed.y),
        1.0 - std::exp(-exposed.z)
    };
    
    // Add halo/glow (cheap bloom effect)
    double glow = std::fmax(toneMapped.x, std::fmax(toneMapped.y, toneMapped.z));
    Vec3 halo = toneMapped * (glow * 0.6);
    toneMapped += halo;
    
    // Final clamp
    return Vec3{
        std::fmin(toneMapped.x, 1.0),
        std::fmin(toneMapped.y, 1.0),
        std::fmin(toneMapped.z, 1.0)
    };
}

int main(int argc, char** argv) {
    // Load space background image
    spaceImage = stbi_load("space_background.jpg", &spaceWidth, &spaceHeight, &spaceChannels, 3);
    if (!spaceImage) {
        std::cerr << "Warning: Failed to load space_background.jpg, using fallback\n";
    } else {
        std::cout << "Loaded space background: " << spaceWidth << "x" << spaceHeight << "\n";
    }

    // "orbit [frames]" -> camera fly-around into frames/. No args -> single still.
    bool orbit = (argc > 1 && std::string(argv[1]) == "orbit");
    int frames = 1;
    if (orbit) {
        TIME_SAMPLES = 1;  // blur is now free across frames, and this is 4x faster
        frames = (argc > 2) ? std::stoi(argv[2]) : 300;
        frames = frames > 0 ? frames : 300;
        std::filesystem::create_directories("frames");
    }

    std::vector<unsigned char> pixels(WIDTH * HEIGHT * 3);

    for (int f = 0; f < frames; f++) {
        double iTime = orbit ? (double)f / (double)frames : 0.0;  // [0,1) = one full orbit

        // Pixels are independent and read only shared consts, so the split is exact:
        // same per-pixel double math in the same order -> bit-identical output to serial.
        #pragma omp parallel for schedule(dynamic, 4)
        for(int y=HEIGHT-1; y>=0; y--) {
            for(int x=0; x<WIDTH; x++) {
                Vec3 col = mainImage(x, y, iTime);
                int idx = (y*WIDTH + x) * 3;
                pixels[idx+0] = (unsigned char)(std::fmin(std::fmax(col.x, 0.0), 1.0) * 255);
                pixels[idx+1] = (unsigned char)(std::fmin(std::fmax(col.y, 0.0), 1.0) * 255);
                pixels[idx+2] = (unsigned char)(std::fmin(std::fmax(col.z, 0.0), 1.0) * 255);
            }
        }

        char path[64];
        if (orbit) std::snprintf(path, sizeof(path), "frames/f_%04d.ppm", f);
        else       std::snprintf(path, sizeof(path), "kerr_hamiltonian.ppm");

        std::ofstream out(path, std::ios::binary);
        out << "P6\n" << WIDTH << " " << HEIGHT << "\n255\n";
        out.write((char*)pixels.data(), pixels.size());
        out.close();

        if (orbit) std::cout << "\rframe " << (f+1) << "/" << frames << std::flush;
    }
    if (orbit) std::cout << "\n";

    // Free image memory
    if (spaceImage) {
        stbi_image_free(spaceImage);
    }

    std::cout << (orbit ? "Rendered orbit frames to frames/\n" : "Rendered kerr_hamiltonian.ppm\n");
    return 0;
}
