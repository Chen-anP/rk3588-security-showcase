// RKNN 模型加载和推理（第 5d 步）：只在板子上、而且模型文件在的时候跑，否则跳过。
// 模型位置：环境变量 RKSEC_MODEL，没有就用仓库自带配置文件（config/rk3588-security.conf）里的 detect.model。
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/config.hpp"
#ifdef RKSEC_HAVE_RKNN
#include <sys/stat.h>

#include "detect/rknn_model.hpp"
#include "face/face_engine.hpp"
#include "face/gallery.hpp"
#include "face/retinaface.hpp"
#include "media/dma_buffer.hpp"
#endif

#ifdef RKSEC_HAVE_RKNN

namespace {

std::string model_path()
{
    if (const char *env = std::getenv("RKSEC_MODEL")) {
        return env;
    }
    std::vector<rksec::ini::Error> errors;
    const auto cfg = rksec::config::load_file(RKSEC_SOURCE_DIR "/config/rk3588-security.conf", errors);
    return cfg ? cfg->detect.model : rksec::config::Detect{}.model;
}

bool usable(const std::string &path)
{
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && stat("/dev/dma_heap/system", &st) == 0;
}

}  // namespace

TEST(RknnModel, LoadsYolo11AndChecksItsOutputs)
{
    const std::string path = model_path();
    if (!usable(path)) {
        GTEST_SKIP() << "没有模型文件 " << path << "（或者没有 dma-heap），设置 RKSEC_MODEL 指定";
    }
    std::string err;
    auto m = rksec::detect::RknnModel::load(path, 0, err);
    ASSERT_TRUE(m) << err;
    EXPECT_TRUE(rksec::detect::check_yolo11(*m, err)) << err;
    EXPECT_EQ(m->input_width(), 640);
    EXPECT_EQ(m->input_height(), 640);
    EXPECT_EQ(m->outputs().size(), 9U);
    EXPECT_FALSE(m->describe().empty());
}

TEST(RknnModel, GrayImageHasNoPeopleAndContextsAgree)
{
    const std::string path = model_path();
    if (!usable(path)) {
        GTEST_SKIP() << "没有模型文件 " << path;
    }
    std::string err;
    auto m0 = rksec::detect::RknnModel::load(path, 0, err);
    ASSERT_TRUE(m0) << err;
    auto m1 = m0->duplicate(1, err);
    ASSERT_TRUE(m1) << err;
    auto in = rksec::media::DmaBuffer::allocate(std::size_t{640} * 640 * 3, err);
    ASSERT_TRUE(in) << err;
    in->begin_cpu_write();
    std::memset(in->data(), 114, in->size());  // 整幅灰色（和 letterbox 的边一样）
    in->end_cpu_write();

    ASSERT_TRUE(m0->run(*in, err)) << err;
    ASSERT_TRUE(m1->run(*in, err)) << err;
    rksec::detect::PostOptions person;
    person.classes = {0};
    const auto d = rksec::detect::yolo11_decode(m0->outputs(), 640, 640, person, err);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_TRUE(d.empty()) << "灰色画面里不应该检测到人";
    // 复制出来的上下文（共享权重，另一个核）结果和原来的一模一样
    for (std::size_t i = 0; i < m0->outputs().size(); ++i) {
        const auto &a = m0->outputs()[i];
        const auto &b = m1->outputs()[i];
        const std::size_t n =
            static_cast<std::size_t>(a.c) * static_cast<std::size_t>(a.h) * static_cast<std::size_t>(a.w);
        EXPECT_EQ(std::memcmp(a.data, b.data, n), 0) << "第 " << i << " 个输出不同";
    }
}

// 人脸检测模型（第 7b 步）：float32 输出模式，输出形状要和先验框个数对得上；灰色画面里没有脸
TEST(RknnModel, RetinaFaceInFloatModeHasNoFaceInGray)
{
    std::vector<rksec::ini::Error> errors;
    const auto cfg = rksec::config::load_file(RKSEC_SOURCE_DIR "/config/rk3588-security.conf", errors);
    const std::string path = cfg ? cfg->face.detect_model : rksec::config::Face{}.detect_model;
    if (!usable(path)) {
        GTEST_SKIP() << "没有人脸检测模型 " << path;
    }
    std::string err;
    auto m = rksec::detect::RknnModel::load(path, 1, err, rksec::detect::OutputMode::float32);
    ASSERT_TRUE(m) << err;
    const auto priors = rksec::face::retinaface_priors(m->input_width(), m->input_height());
    std::vector<std::vector<int>> dims;
    for (const auto &t : m->float_outputs()) {
        dims.push_back(t.dims);
    }
    ASSERT_TRUE(rksec::face::check_retinaface_outputs(dims, priors.size(), err)) << err;
    auto in = rksec::media::DmaBuffer::allocate(
        static_cast<std::size_t>(m->input_width()) * static_cast<std::size_t>(m->input_height()) * 3, err);
    ASSERT_TRUE(in) << err;
    in->begin_cpu_write();
    std::memset(in->data(), 114, in->size());
    in->end_cpu_write();
    ASSERT_TRUE(m->run(*in, err)) << err;
    const auto &o = m->float_outputs();
    const auto faces = rksec::face::retinaface_decode(o[0].data.data(), o[1].data.data(), o[2].data.data(), priors,
                                                      m->input_width(), m->input_height(), {});
    EXPECT_TRUE(faces.empty()) << "灰色画面里不应该有脸";
}

// 人脸识别的两个模型（第 7c 步）：能加载、形状对；同一张图两次的特征一样（推理是确定的）；
// 两张不同的图特征不一样（模型不是输出常数）
TEST(FaceEngine, LoadsBothModelsAndGivesStableEmbeddings)
{
    std::vector<rksec::ini::Error> errors;
    const auto cfg = rksec::config::load_file(RKSEC_SOURCE_DIR "/config/rk3588-security.conf", errors);
    const rksec::config::Face fc = cfg ? cfg->face : rksec::config::Face{};
    if (!usable(fc.detect_model) || !usable(fc.recognize_model)) {
        GTEST_SKIP() << "没有人脸模型 " << fc.detect_model << " 或 " << fc.recognize_model;
    }
    std::string err;
    auto e = rksec::face::FaceEngine::open(fc, err);
    ASSERT_TRUE(e) << err;
    std::vector<rksec::face::Face> faces;
    auto &in = e->detect_input();
    in.begin_cpu_write();
    std::memset(in.data(), 114, in.size());
    in.end_cpu_write();
    ASSERT_TRUE(e->detect(faces, err)) << err;
    EXPECT_TRUE(faces.empty());

    // 关键点正好在参考位置：对齐就是原样拷贝 112×112
    rksec::face::Face f;
    f.landmarks = rksec::face::kArcFaceRef;
    std::vector<std::uint8_t> img(112 * 112 * 3);
    for (std::size_t i = 0; i < img.size(); ++i) {
        img[i] = static_cast<std::uint8_t>((i * 37 + i / 336 * 11) % 256);
    }
    const rksec::face::ImageView v{img.data(), 112, 112, 112 * 3, rksec::face::PixelFormat::rgb888};
    std::vector<float> a;
    std::vector<float> b;
    float norm = 0;
    ASSERT_TRUE(e->embed(v, f, a, err, nullptr, &norm)) << err;
    ASSERT_EQ(a.size(), rksec::face::kEmbeddingSize);
    EXPECT_GT(norm, 0.0F);
    ASSERT_TRUE(e->embed(v, f, b, err)) << err;
    EXPECT_GT(rksec::face::cosine(a, b), 0.9999F) << "同一张图两次";
    std::reverse(img.begin(), img.end());
    ASSERT_TRUE(e->embed(v, f, b, err)) << err;
    EXPECT_LT(rksec::face::cosine(a, b), 0.99F) << "换了一张图，特征应该变";
}

#else

TEST(RknnModel, NotBuiltWithoutRknn)
{
    GTEST_SKIP() << "这次编译没有 RKNN（CI、电脑上正常）";
}

#endif
