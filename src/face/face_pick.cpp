#include "face/face_pick.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rksec::face {

PickResult pick_face_detail(const std::vector<Face> &faces, const detect::Detection &person)
{
    PickResult r;
    const float pw = person.x2 - person.x1;
    const float ph = person.y2 - person.y1;
    if (pw <= 0 || ph <= 0) {
        return r;
    }
    const float pcx = (person.x1 + person.x2) / 2;
    float best_cost = std::numeric_limits<float>::max();
    float nearest_cost = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < faces.size(); ++i) {
        const detect::Detection &b = faces[i].box;
        const float wr = (b.x2 - b.x1) / pw;
        const float dx = std::fabs((b.x1 + b.x2) / 2 - pcx) / pw;
        const float dy = ((b.y1 + b.y2) / 2 - person.y1) / ph;
        const float cost = dy + 0.5F * dx;
        const float max_depth = wr >= kHeadOnlyRatio ? 1.0F : kMaxDepthY;  // 人框只剩头时不限深度
        const bool eligible = wr >= kMinWidthRatio && dx <= kMaxOffsetX && dy <= max_depth;
        // nearest：有符合条件的就是挑中的那张；都不符合时是代价最小的那张
        const bool better_nearest = r.index < 0 && (eligible || cost < nearest_cost);
        if (eligible && cost < best_cost) {
            best_cost = cost;
            r.index = static_cast<int>(i);
        }
        if (better_nearest || (eligible && r.index == static_cast<int>(i))) {
            nearest_cost = cost;
            r.nearest = static_cast<int>(i);
            r.offset_x = dx;
            r.depth_y = dy;
            r.width_ratio = wr;
        }
    }
    return r;
}

int pick_face(const std::vector<Face> &faces, const detect::Detection &person)
{
    return pick_face_detail(faces, person).index;
}

Pose estimate_pose(const Face &f)
{
    const Point &le = f.landmarks[0];
    const Point &re = f.landmarks[1];
    const Point &nose = f.landmarks[2];
    const Point &lm = f.landmarks[3];
    const Point &rm = f.landmarks[4];
    Pose p;
    const float eye_dx = re.x - le.x;
    const float eye_y = (le.y + re.y) / 2;
    const float mouth_y = (lm.y + rm.y) / 2;
    const float span = mouth_y - eye_y;
    if (std::fabs(eye_dx) < 1e-3F || span < 1e-3F) {
        return p;
    }
    p.yaw = (nose.x - (le.x + re.x) / 2) / std::fabs(eye_dx);
    p.pitch = (nose.y - eye_y) / span;
    p.valid = true;
    return p;
}

Quality face_quality(const Face &face, const Pose &pose, int frame_w, int frame_h, const QualityLimits &lim)
{
    // 正常的脸 |yaw| 不会超过 1.5（鼻尖跑到眼睛外面很远）、pitch 在 -1～2 之间（鼻尖在眼睛和嘴附近）
    if (!pose.valid || std::fabs(pose.yaw) > 1.5F || pose.pitch < -1.0F || pose.pitch > 2.0F) {
        return Quality::bad_landmarks;
    }
    if (face.box.score < lim.min_score) {
        return Quality::low_score;
    }
    // 看 5 个关键点（眼、鼻、嘴角）在不在画面里，不看脸框：离得近时额头、下巴被切掉一点不影响识别
    // （11/06 第二轮实测按脸框判断挡掉了 20 次正常的脸）；关键点出了画面说明脸只拍到一部分
    const float e = std::max(static_cast<float>(lim.edge), 0.05F * (face.box.x2 - face.box.x1));
    for (const Point &p : face.landmarks) {
        if (p.x < e || p.y < e || p.x > static_cast<float>(frame_w) - e || p.y > static_cast<float>(frame_h) - e) {
            return Quality::at_edge;
        }
    }
    if (std::fabs(pose.yaw) > lim.max_yaw) {
        return Quality::turned;
    }
    if (pose.pitch < lim.min_pitch || pose.pitch > lim.max_pitch) {
        return Quality::tilted;
    }
    return Quality::ok;
}

const char *quality_text(Quality q)
{
    switch (q) {
    case Quality::ok:
        return "合格";
    case Quality::bad_landmarks:
        return "关键点异常";
    case Quality::low_score:
        return "检测分数低";
    case Quality::at_edge:
        return "贴着画面边";
    case Quality::turned:
        return "转头太大";
    case Quality::tilted:
        return "抬头低头太大";
    }
    return "未知";
}

}  // namespace rksec::face
