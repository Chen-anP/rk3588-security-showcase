# 门禁状态机（9d-1）

本阶段只提供纯逻辑模块和模拟测试，不改数据库，不接入实际识别、屏幕、录像或开门控制。摄像头与安防流水线不由此模块启停。

## 状态与时间

- idle：等待红外移动事件；红外不可用时，新检测帧中有人也能开始会话。
- identifying：仅接收当前 session 的新识别样本，默认超时 10 秒。
- granted：连续 2 个独立样本匹配同一 track 和人员键，发出一次 grant；保持 3 秒。
- denied：连续 2 个独立样本均为合格陌生人，发出一次 deny；保持 3 秒。
- cooldown：保持 5 秒，再允许开始会话。

时间由调用方传入 steady_clock，倒退时间的整次输入被忽略。无新结果时也应定时调用 update，才能推进超时和冷却。识别截止时间处优先超时，不采用同一时刻才到达的识别结果。

## 输入契约

- motion 是移动触发事件，不能每个 tick 都把持续高电平当新事件。
- pir_available 是当前可用状态，每次调用需传入真实状态；故障只标记降级，不直接判陌生人。
- present_tracks 缺省表示没有新检测帧，空列表表示新帧没有人。画面持续确认无人达到 5 秒才解除人员去重。为避免编号切换导致重复通行，画面仍有人时保留去重；多人场景中此策略可能延迟已通行人员再次通行。
- Sample 是原始的新推理结果，不是 IdentityBook 缓存结论。识别任务启动时绑定当前 session，每个独立任务有严格递增的 sequence；重复/倒序 sequence、旧 session 不计数。
- no_face 包括没有找到脸、质量不合格；会打断候选连续计数，不生成 deny。
- known 使用稳定人员键（后续 DB ID），不能以显示名或 track 当数据库主键；键非空且最多 128 字节。
- 不同人员、不同 track 或不同 verdict 不能拼成连续确认。

## 动作与资源边界

Result 的 grant/deny/timeout/suppressed/capacity 为一次性动作，外层只能在 grant 时请求模拟开门并写通行记录；deny 后的录像请求由 9d-3 适配，现有安防录像逻辑保持独立。状态结果用于提示和超时收尾。

已通行人员去重表最多 64 项，达到上限时不通行，返回 capacity；不能把容量不足当陌生人。单个 Machine 仅由一个线程更新；跨线程输入须由外层串行投递。

## 验证场景

模拟时钟覆盖：独立连续样本、重复样本、无脸打断、人员与 track 混淆、识别超时、截止边界、陌生人确认、冷却、旧会话、同人换编号去重、无人重置、红外降级、时间倒退、无效人员键和容量上限等。

本阶段验证的是模块逻辑，不代表已完成实际门禁业务验收。下一步 9d-2 实现数据库 version 3 和通行记录，再由 9d-3 接入实际输入与界面。


## 构建与验证（2026-11-09）

应用包后显式重新配置，再构建，不能只依赖文件时间触发增量重配置：

```bash
cmake --preset ci
cmake --build --preset ci -j4
ctest --preset ci 2>&1 | tee -i /tmp/access-ci-test.txt
./build/ci/tests/rksec_tests --gtest_filter='AccessMachine.*' --gtest_list_tests
```

板子 CI 317 项全部通过；新增 AccessMachine 模拟测试 16 项，ASan/TSan 各 16 项全部通过。格式和真实配置 clang-tidy 通过。
WSL 新增 16 项 ASan/UBSan 通过；WSL TSan 在启动前出现 unexpected memory mapping，未执行测试，已由板子 TSan 补验。

检查时曾发现包的文件时间戳导致增量构建没有加载新增 CMake 项，旧 CI 301 项和 sanitizer 零项运行均作废；显式重新配置后检查新增用例数量并重跑，以上结果才是有效验收。
