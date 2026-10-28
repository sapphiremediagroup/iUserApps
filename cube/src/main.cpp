#include <cstdint.hpp>
#include <ui/window.hpp>

namespace {

constexpr int kFrameIntervalMs = 16;
constexpr ui::Color kColorBackground = ui::rgb(0x091218);
constexpr ui::Color kColorCube = ui::rgb(0x3ba4d8);
constexpr ui::Color kColorCubeBright = ui::rgb(0xd7f2e6);

constexpr int kFixedShift = 14;
constexpr int kFixedOne = 1 << kFixedShift;

// Small incremental rotations, fixed point:
// cos(0.030) ~= 0.99955, sin(0.030) ~= 0.03000
// cos(0.021) ~= 0.99978, sin(0.021) ~= 0.02100
constexpr int kRotXCos = 16377;
constexpr int kRotXSin = 492;
constexpr int kRotYCos = 16380;
constexpr int kRotYSin = 344;

struct Vec3 {
    int x;
    int y;
    int z;
};

struct RotationState {
    int cosX;
    int sinX;
    int cosY;
    int sinY;
};

int fixed_mul(int a, int b) {
    return static_cast<int>((static_cast<std::int64_t>(a) * static_cast<std::int64_t>(b)) >> kFixedShift);
}

void update_rotation(RotationState& rot) {
    const int oldCosX = rot.cosX;
    const int oldSinX = rot.sinX;
    const int oldCosY = rot.cosY;
    const int oldSinY = rot.sinY;

    rot.cosX = fixed_mul(oldCosX, kRotXCos) - fixed_mul(oldSinX, kRotXSin);
    rot.sinX = fixed_mul(oldSinX, kRotXCos) + fixed_mul(oldCosX, kRotXSin);

    rot.cosY = fixed_mul(oldCosY, kRotYCos) - fixed_mul(oldSinY, kRotYSin);
    rot.sinY = fixed_mul(oldSinY, kRotYCos) + fixed_mul(oldCosY, kRotYSin);
}

Vec3 rotate_vertex(const Vec3& v, const RotationState& rot) {
    // Around X, then around Y.
    Vec3 rx = {};
    rx.x = v.x;
    rx.y = fixed_mul(v.y, rot.cosX) - fixed_mul(v.z, rot.sinX);
    rx.z = fixed_mul(v.y, rot.sinX) + fixed_mul(v.z, rot.cosX);

    Vec3 ry = {};
    ry.x = fixed_mul(rx.x, rot.cosY) + fixed_mul(rx.z, rot.sinY);
    ry.y = rx.y;
    ry.z = -fixed_mul(rx.x, rot.sinY) + fixed_mul(rx.z, rot.cosY);
    return ry;
}

// Perspective onto a window of `size`, centred, scaled with its smaller side
// (the cube fills a 640x480 window as before).
ui::Point project_vertex(const Vec3& v, ui::Size size) {
    constexpr int cameraDistance = 420;
    const int smaller = size.width < size.height ? size.width : size.height;
    const int projectionScale = 260 * smaller / 480;

    int z = v.z + cameraDistance;
    if (z < 1) {
        z = 1;
    }
    return { size.width / 2 + (v.x * projectionScale) / z, size.height / 2 - (v.y * projectionScale) / z };
}

class CubeWindow : public ui::Window {
    RotationState rotation_ = { kFixedOne, 0, kFixedOne, 0 };

    void on_create() override {
        set_timer(kFrameIntervalMs);
    }

    void on_timer(int) override {
        update_rotation(rotation_);
        invalidate();
    }

    void on_paint(ui::Canvas& canvas, const ui::Rect&) override {
        const ui::Size size = client_size();
        canvas.fill({ 0, 0, size.width, size.height }, kColorBackground);

        constexpr int s = 120;
        const Vec3 vertices[8] = {
            {-s, -s, -s}, { s, -s, -s}, { s,  s, -s}, {-s,  s, -s},
            {-s, -s,  s}, { s, -s,  s}, { s,  s,  s}, {-s,  s,  s},
        };
        const int edges[12][2] = {
            {0, 1}, {1, 2}, {2, 3}, {3, 0},
            {4, 5}, {5, 6}, {6, 7}, {7, 4},
            {0, 4}, {1, 5}, {2, 6}, {3, 7},
        };

        ui::Point projected[8];
        for (int i = 0; i < 8; ++i) {
            projected[i] = project_vertex(rotate_vertex(vertices[i], rotation_), size);
        }
        for (int i = 0; i < 12; ++i) {
            canvas.line(projected[edges[i][0]], projected[edges[i][1]], i < 4 ? kColorCubeBright : kColorCube);
        }
    }
};

}  // namespace

int main(int, char**) {
    return ui::run<CubeWindow>({ .width = 640, .height = 480, .title = "Cube" });
}
