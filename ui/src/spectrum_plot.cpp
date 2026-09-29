#include "spectrum_plot.hpp"

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

#include "theme.hpp"

namespace libs::ui {

namespace {
constexpr int kLeftMargin = 62;    // 留给纵轴刻度（五位数，整数）
constexpr int kRightMargin = 10;
constexpr int kTopMargin = 26;     // 留给图例
constexpr int kBottomMargin = 26;  // 留给横轴刻度

// 纵轴固定 5 条刻度线：0 / 16000 / 32000 / 48000 / 64000。
// 与厂家参考实现一致：**纵轴不自动缩放**，这样峰高在帧与帧之间可比。
constexpr double kYMax = 64000.0;
constexpr int kYTicks = 4;
}  // namespace

SpectrumPlot::SpectrumPlot(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(260);
    setAutoFillBackground(false);
    // 每帧重画，不搞动画 —— 动画只会拖慢。
    setAttribute(Qt::WA_OpaquePaintEvent, true);
}

void SpectrumPlot::set_segments(QVector<Segment> segments) {
    double xlo = std::numeric_limits<double>::infinity();
    double xhi = -std::numeric_limits<double>::infinity();
    double ymax = 0.0;
    for (const Segment& s : segments) {
        if (s.points.isEmpty()) continue;
        xlo = std::min(xlo, s.points.first().x());
        xhi = std::max(xhi, s.points.last().x());
        for (const QPointF& p : s.points) ymax = std::max(ymax, p.y());
    }
    if (xlo <= xhi) { x_lo_ = xlo; x_hi_ = xhi; }

    // 纵轴：默认**固定满量程**（照厂家）。勾了自动才跟数据走，
    // 但上限仍锁在满量程 —— 超过就是饱和，不该让坐标轴跟着涨。
    if (lock_y_) {
        // 锁住当前上限，不动
    } else if (auto_y_) {
        double hi = (ymax > 0.0) ? ymax * 1.05 : 1000.0;
        y_hi_ = std::clamp(hi, 1000.0, kYMax);
    } else {
        y_hi_ = kYMax;
    }

    segments_ = std::move(segments);
    update();   // 只排一次重绘，不做任何控件结构变更
}

void SpectrumPlot::set_dimmed(bool dimmed) {
    if (dimmed_ == dimmed) return;
    dimmed_ = dimmed;
    update();
}

void SpectrumPlot::set_auto_y(bool on) { auto_y_ = on; }
void SpectrumPlot::set_lock_y(bool on) { lock_y_ = on; }
void SpectrumPlot::set_y_max(double hi) { y_hi_ = std::clamp(hi, 1.0, kYMax); update(); }

void SpectrumPlot::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor(0x0A, 0x0E, 0x13));

    const QRectF r(kLeftMargin, kTopMargin,
                   std::max(10, width() - kLeftMargin - kRightMargin),
                   std::max(10, height() - kTopMargin - kBottomMargin));

    QFont small(QStringLiteral("Microsoft YaHei UI"), 9);
    p.setFont(small);
    const QFontMetrics fm(small);

    // ── 网格 + 纵轴刻度（整数） ──────────────────────────────────────────
    const double yspan = (y_hi_ > y_lo_) ? (y_hi_ - y_lo_) : 1.0;
    p.setPen(QPen(QColor(0x22, 0x2B, 0x35), 1));
    for (int i = 0; i <= kYTicks; ++i) {
        const double v = y_lo_ + yspan * i / kYTicks;
        const double y = r.bottom() - r.height() * i / kYTicks;
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        const QString label = QString::number(static_cast<qint64>(v));
        p.setPen(theme::color::textFaint());
        p.drawText(QRectF(0, y - 8, kLeftMargin - 6, 16),
                   Qt::AlignRight | Qt::AlignVCenter, label);
        p.setPen(QPen(QColor(0x22, 0x2B, 0x35), 1));
    }

    // ── 横轴刻度（波长）──────────────────────────────────────────────────
    const int xTicks = 6;
    for (int i = 0; i <= xTicks; ++i) {
        const double v = x_lo_ + (x_hi_ - x_lo_) * i / xTicks;
        const double x = r.left() + r.width() * i / xTicks;
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        p.setPen(theme::color::textFaint());
        p.drawText(QRectF(x - 40, r.bottom() + 3, 80, 18),
                   Qt::AlignHCenter | Qt::AlignTop,
                   QString::number(static_cast<qint64>(std::lround(v))));
        p.setPen(QPen(QColor(0x22, 0x2B, 0x35), 1));
    }
    p.setPen(theme::color::border());
    p.drawRect(r);

    // ── 曲线：QPainter::drawPolyline 直接画，一笔一段 ─────────────────────
    //
    // 这就是 Qwt 底层那一步。点集已经是调用方给好的数组，这里只做
    // "波长 -> 像素"的线性映射；映射缓冲复用成员，避免每帧重新分配。
    const double xspan = (x_hi_ > x_lo_) ? (x_hi_ - x_lo_) : 1.0;
    QPolygonF poly;
    for (const Segment& s : segments_) {
        if (s.points.size() < 2) continue;
        poly.resize(s.points.size());
        for (int i = 0; i < s.points.size(); ++i) {
            const QPointF& src = s.points[i];
            const double px = r.left() + (src.x() - x_lo_) / xspan * r.width();
            double py = r.bottom() - (src.y() - y_lo_) / yspan * r.height();
            py = std::clamp(py, r.top() - 2.0, r.bottom() + 2.0);
            poly[i] = QPointF(px, py);
        }
        QColor c = dimmed_ ? theme::color::idle() : s.color;
        p.setPen(QPen(c, 1.1));
        p.setRenderHint(QPainter::Antialiasing, true);
        p.drawPolyline(poly);
    }

    // ── 图例：四路的序列号，按各自颜色 ───────────────────────────────────
    int lx = r.left() + 4;
    for (const Segment& s : segments_) {
        const QColor c = dimmed_ ? theme::color::idle() : s.color;
        p.setPen(QPen(c, 2));
        p.drawLine(QPointF(lx, kTopMargin / 2.0), QPointF(lx + 14, kTopMargin / 2.0));
        p.setPen(theme::color::textDim());
        p.drawText(QRectF(lx + 18, kTopMargin / 2.0 - 8, 110, 16),
                   Qt::AlignLeft | Qt::AlignVCenter, s.name);
        lx += 130;
        if (lx > r.right() - 120) break;
    }
}

}  // namespace libs::ui
