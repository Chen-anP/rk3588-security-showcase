#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

// 页面导航：记录当前在哪一页、按“返回”回到哪一页。只管逻辑，不碰界面库，所以能单独测试。
// 界面那边注册 on_change，收到通知后切换显示的页面。
//
// 规则：
// - go(p)：进入 p，当前页压栈；已经在 p 就什么都不做（连点两下不会压两次）
// - back()：回到上一页；已经在首页返回 false
// - home()：直接回首页，清空历史（比如长时间无人操作时）
// - 历史最多 max_depth 层，超过就丢最早的，防止来回点击让栈无限增长
namespace rksec::ui {

enum class Page : std::uint8_t { home, events, settings, about };

const char *page_name(Page p);

class Router {
  public:
    using Listener = std::function<void(Page from, Page to)>;

    explicit Router(std::size_t max_depth = 8);

    void on_change(Listener l) { listener_ = std::move(l); }

    void go(Page p);
    bool back();
    void home();

    Page current() const { return current_; }
    std::size_t depth() const { return history_.size(); }

  private:
    void switch_to(Page p);

    std::size_t max_depth_;
    Page current_ = Page::home;
    std::vector<Page> history_;
    Listener listener_;
};

}  // namespace rksec::ui
