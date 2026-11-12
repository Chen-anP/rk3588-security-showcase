#include "ui/router.hpp"

namespace rksec::ui {

const char *page_name(Page p)
{
    switch (p) {
    case Page::home:
        return "home";
    case Page::events:
        return "events";
    case Page::settings:
        return "settings";
    case Page::about:
        return "about";
    }
    return "?";
}

Router::Router(std::size_t max_depth) : max_depth_(max_depth == 0 ? 1 : max_depth) {}

void Router::switch_to(Page p)
{
    const Page from = current_;
    current_ = p;
    if (listener_) {
        listener_(from, p);
    }
}

void Router::go(Page p)
{
    if (p == current_) {
        return;
    }
    if (p == Page::home) {
        home();
        return;
    }
    history_.push_back(current_);
    if (history_.size() > max_depth_) {
        history_.erase(history_.begin());
    }
    switch_to(p);
}

bool Router::back()
{
    if (history_.empty()) {
        return false;
    }
    const Page p = history_.back();
    history_.pop_back();
    switch_to(p);
    return true;
}

void Router::home()
{
    history_.clear();
    if (current_ != Page::home) {
        switch_to(Page::home);
    }
}

}  // namespace rksec::ui
