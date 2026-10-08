// 帧队列测试。**故意只链 frame/frame_bus/json/time_util 四个纯内存文件**：
// 帧队列必须保持零设备依赖，这条链接线就是它的看守 —
// 往里加任何设备调用都会直接链接失败，而不是靠 code review 发现。
//
// 这里守的是 frame_queue.hpp 文件头写下的纪律：
//   通道缺失是第三态 / 丢最旧不阻塞生产者 / 帧里不做显示加工 /
//   帧号与丢帧分开 / 订阅生命周期。

#include "check.hpp"


#include "libs/business/measurement/frame.hpp"
#include "libs/business/measurement/frame_bus.hpp"

namespace json = business::json;
namespace measurement = business::measurement;   // to_json 走限定名，与界面对齐
using business::measurement::ChannelFrame;
using business::measurement::ChannelState;
using business::measurement::FrameQueue;
using business::measurement::SpectrumFrame;

namespace {

SpectrumFrame make_frame(std::int64_t index, bool laser_shot = true) {
    SpectrumFrame f;
    f.frame_index = index;
    f.shot_id = static_cast<std::uint32_t>(index + 100);
    f.laser_shot = laser_shot;
    f.wall_clock_ms = 1000 + index;
    f.captured_monotonic_ms = 500 + index;
    f.hardware_synchronized = true;
    f.requested_sync = true;
    f.period_ms = 12.5;
    f.timeout_ms = 3000;
    ChannelFrame c;
    c.serial = "S1";
    c.state = ChannelState::present;
    c.pixels = 3;
    c.wavelength_nm = {500.0, 501.0, 502.0};
    c.counts = {10.0, 20.0, 30.0};
    f.channels.push_back(c);
    return f;
}

}  // namespace

TEST_MAIN_BEGIN()
    // ── 订阅与取帧 ───────────────────────────────────────────────────────
    CASE(subscribe_returns_distinct_ids); {
        FrameQueue q;
        const std::uint64_t a = q.subscribe();
        const std::uint64_t b = q.subscribe();
        CHECK(a != b);
        CHECK(q.status()["subscribers"].as_number() == 2.0);
    }

    CASE(try_pop_on_empty_queue_is_not_an_error); {
        FrameQueue q;
        const std::uint64_t id = q.subscribe();
        SpectrumFrame f;
        CHECK(!q.try_pop(id, f));       // false = "暂时没有"，不是错误
        CHECK(q.pending(id) == 0);
    }

    CASE(try_pop_with_unknown_subscriber_returns_false); {
        FrameQueue q;
        SpectrumFrame f;
        CHECK(!q.try_pop(9999, f));     // 订阅已退订/从不存在：不能崩、不能拿到别人的帧
    }

    CASE(published_frame_reaches_every_subscriber); {
        FrameQueue q;
        const std::uint64_t a = q.subscribe();
        const std::uint64_t b = q.subscribe();
        q.publish(make_frame(0));
        SpectrumFrame fa;
        SpectrumFrame fb;
        CHECK(q.try_pop(a, fa));
        CHECK(q.try_pop(b, fb));
        CHECK(fa.frame_index == 0);
        CHECK(fb.frame_index == 0);
        CHECK(fa.channels.size() == 1);
        CHECK(fa.channels[0].counts.size() == 3);
        CHECK(fa.laser_shot);
        CHECK(q.pending(a) == 0);
    }

    CASE(frames_come_out_in_order); {
        FrameQueue q;
        const std::uint64_t id = q.subscribe();
        q.publish(make_frame(0));
        q.publish(make_frame(1));
        q.publish(make_frame(2));
        for (std::int64_t expect = 0; expect < 3; ++expect) {
            SpectrumFrame f;
            CHECK(q.try_pop(id, f));
            CHECK(f.frame_index == expect);
        }
    }

    // ── 背压：丢最旧，绝不阻塞生产者 ─────────────────────────────────────
    CASE(full_queue_drops_the_oldest_frame_not_the_newest); {
        FrameQueue::Options o;
        o.subscriber_queue_frames = 2;
        FrameQueue q(o);
        const std::uint64_t id = q.subscribe();
        q.publish(make_frame(0));
        q.publish(make_frame(1));
        q.publish(make_frame(2));      // 满了：丢 frame 0
        q.publish(make_frame(3));      // 又满：丢 frame 1

        CHECK(q.pending(id) == 2);
        CHECK(q.dropped_frames() == 2);
        SpectrumFrame f;
        CHECK(q.try_pop(id, f));
        CHECK(f.frame_index == 2);     // 留下的是**新**的
        CHECK(q.try_pop(id, f));
        CHECK(f.frame_index == 3);
    }

    CASE(one_slow_subscriber_does_not_starve_others); {
        FrameQueue::Options o;
        o.subscriber_queue_frames = 1;
        FrameQueue q(o);
        q.subscribe();                  // 慢的那位：一帧都不取
        const std::uint64_t fast = q.subscribe();

        q.publish(make_frame(0));
        // fast 把自己那帧取走，慢的那位继续不取。
        SpectrumFrame taken;
        CHECK(q.try_pop(fast, taken));
        q.publish(make_frame(1));
        q.publish(make_frame(2));

        // 慢的那位一直在丢，但快的仍拿到最新帧 —— 一个卡住的 UI
        // 不允许把采集节奏拖住，更不允许让别人也看不到谱。
        CHECK(q.dropped_frames() >= 1);
        CHECK(q.try_pop(fast, taken));
        CHECK(taken.frame_index == 2);
    }

    CASE(publish_does_not_block_on_a_backlogged_subscriber); {
        // 只是把"publish 不碰任何阻塞原语"变成可观测的行为：连续灌满几轮，
        // 帧号必须严格递增且 pending 不超过上限。
        FrameQueue::Options o;
        o.subscriber_queue_frames = 3;
        FrameQueue q(o);
        const std::uint64_t id = q.subscribe();
        for (std::int64_t i = 0; i < 50; ++i) q.publish(make_frame(i));
        CHECK(q.pending(id) == 3);
        SpectrumFrame f;
        CHECK(q.try_pop(id, f));
        CHECK(f.frame_index == 47);    // 只保最新的三帧
    }

    // ── 帧号与丢帧：两件事，不许互相污染 ────────────────────────────────
    CASE(dropped_frames_do_not_shift_producer_frame_index); {
        FrameQueue::Options o;
        o.subscriber_queue_frames = 1;
        FrameQueue q(o);
        const std::uint64_t id = q.subscribe();
        for (std::int64_t i = 0; i < 4; ++i) q.publish(make_frame(i));
        CHECK(q.dropped_frames() == 3);
        // 丢帧是**显示侧**的事：生产者的帧号仍然是 0,1,2,3 原样上送，
        // UI 才能靠它判断"漏了几帧"。曾经帧号是拿
        // `dropped + captured + failed` 算出来的，结果漏显示一次就让
        // 后面所有帧号错位，UI 永远显示"跳号"。
        SpectrumFrame f;
        CHECK(q.try_pop(id, f));
        CHECK(f.frame_index == 3);
    }

    // ── 退订 ────────────────────────────────────────────────────────────
    CASE(unsubscribe_stops_delivery_and_is_idempotent); {
        FrameQueue q;
        const std::uint64_t id = q.subscribe();
        q.publish(make_frame(0));
        q.unsubscribe(id);
        q.unsubscribe(id);             // 重复退订必须安全
        q.publish(make_frame(1));
        CHECK(q.status()["subscribers"].as_number() == 0.0);
        SpectrumFrame f;
        CHECK(!q.try_pop(id, f));
    }

    CASE(unsubscribing_does_not_disturb_other_subscribers); {
        FrameQueue q;
        const std::uint64_t a = q.subscribe();
        const std::uint64_t b = q.subscribe();
        q.publish(make_frame(0));
        q.unsubscribe(a);
        q.publish(make_frame(1));
        // b 没被退订，所以它**两帧都该收到**：帧 0 是退订前就在它队列里的，
        // 帧 1 是退订之后才发布的。要断言的是"b 的投递没有被打断"，
        // 因此按顺序取两帧，而不是只看最后一帧。
        SpectrumFrame fb;
        CHECK(q.try_pop(b, fb));
        CHECK(fb.frame_index == 0);
        CHECK(q.try_pop(b, fb));
        CHECK(fb.frame_index == 1);
    }

    CASE(clear_pending_empties_queues_but_keeps_frame_index); {
        FrameQueue q;
        const std::uint64_t id = q.subscribe();
        q.publish(make_frame(0));
        q.publish(make_frame(1));
        q.clear_pending();
        CHECK(q.pending(id) == 0);
        // 帧号跨 start/stop 保持连续，UI 才能用同一个计数器去重；
        // 清队列是"这些旧帧没意义了"，不是"重新计数"。
        q.publish(make_frame(2));
        SpectrumFrame f;
        CHECK(q.try_pop(id, f));
        CHECK(f.frame_index == 2);
    }

    // ── JSON：字段名是 UI 契约 ───────────────────────────────────────────
    CASE(json_marks_absent_channel_with_a_reason_not_zero_counts); {
        SpectrumFrame f;
        ChannelFrame missing;
        missing.serial = "S2";
        missing.state = ChannelState::absent;
        missing.reason = "该通道本帧无数据";
        f.channels.push_back(missing);

        const json::Value v = measurement::to_json(f);
        const json::Value& c = v["channels"][0];
        CHECK(c["serial"].as_string() == "S2");
        CHECK(c["state"].as_string() == "absent");
        CHECK(c["reason"].as_string() == "该通道本帧无数据");
        // 关键：absent 时**不出现** counts/wavelength_nm。
        // 发了空数组，UI 会把它画成一条平的线，看起来"这一路没信号"，
        // 而不是"这一路根本没数据"。
        CHECK(!c.has("counts"));
        CHECK(!c.has("wavelength_nm"));
    }

    CASE(json_carries_laser_shot_and_sync_facts_separately); {
        SpectrumFrame f = make_frame(7, /*laser_shot=*/true);
        f.requested_sync = false;      // 请求不同步，但硬件报告同步了：两件事不合并
        const json::Value v = measurement::to_json(f);
        CHECK(v["laser_shot"].as_bool());
        CHECK(v["hardware_synchronized"].as_bool());
        CHECK(!v["requested_sync"].as_bool());
        CHECK(v["frame_index"].as_number() == 7.0);
    }

    CASE(json_marks_background_preview_frames_as_not_laser_shots); {
        // 空闲预览帧必须能被一眼区分 —— 否则操作员会把背景谱当测量谱。
        const json::Value v = measurement::to_json(make_frame(0, /*laser_shot=*/false));
        CHECK(!v["laser_shot"].as_bool());
    }

    CASE(json_reports_capture_error_only_when_present); {
        SpectrumFrame ok = make_frame(0);
        CHECK(measurement::to_json(ok)["ok"].as_bool());
        CHECK(!measurement::to_json(ok).has("capture_error_code"));

        SpectrumFrame bad;
        bad.capture_error_code = "capture_timeout";
        bad.capture_error_message = "等待激光 TTL 超时";
        const json::Value v = measurement::to_json(bad);
        CHECK(!v["ok"].as_bool());
        CHECK(v["capture_error_code"].as_string() == "capture_timeout");
        CHECK(v["capture_error_message"].as_string() == "等待激光 TTL 超时");
    }

    CASE(json_leaves_wavelength_and_counts_raw); {
        // 帧里**不做**归一化 / 基线扣除 / 平均。UI 要归一化自己算 ——
        // 把显示口径写死在业务层，改口径就得改库。
        const json::Value v = measurement::to_json(make_frame(0));
        const json::Value& c = v["channels"][0];
        CHECK(c["wavelength_nm"][0].as_number() == 500.0);
        CHECK(c["counts"][0].as_number() == 10.0);
        CHECK(c["pixels"].as_number() == 3.0);
    }

    CASE(status_does_not_claim_to_collect); {
        // status 必须如实说"我不采集"。以前它报 running/spectrometer_open，
        // 于是 UI 显示"流在跑"而实际没有任何采集发生 ——
        // 那种"看起来一切正常、一帧都没有"最难排障。
        FrameQueue q;
        const json::Value s = q.status();
        CHECK(!s.has("running"));
        CHECK(s["spectrometer_open"].is_null());
        CHECK(s["note"].as_string().find("不采集") != std::string::npos);
    }

    CASE(status_counts_delivered_and_dropped_frames); {
        FrameQueue::Options o;
        o.subscriber_queue_frames = 1;
        FrameQueue q(o);
        q.subscribe();
        q.publish(make_frame(0));
        q.publish(make_frame(1));
        const json::Value s = q.status();
        CHECK(s["delivered_frames"].as_number() == 2.0);
        CHECK(s["dropped_frames"].as_number() == 1.0);
        CHECK(s["subscribers"].as_number() == 1.0);
    }

    TEST_MAIN_RETURN()
