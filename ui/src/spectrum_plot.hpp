#pragma once

// 自绘光谱图（`ui/src/spectrum_plot.hpp`）。
//
// **为什么不用 QtCharts**：`QLineSeries` 每帧必须把点**拷贝**进控件内部，
// 四路 2048 点就是 8192 次拷贝，再加上图例与坐标轴的重排 —— 显示端跟不上
// 采集，stream 那边的 dropped 一路涨到上千。
//
// 厂家参考实现（SDK 的 `Qt_demo_multichannel/plot.cpp`，基于 Qwt）是这么干的：
//
//     setAxisScale(yLeft, -500.0, 66000.0);          // 纵轴钉死在满量程
//     curve[i] = new QwtPlotCurve(...);              // 曲线只建一次
//     curve[i]->setSamples(&m_Lambda[i][s], &m_Spectrum[i], n);   // 零拷贝
//     replot();                                      // 只重画，不重建
//
// 本类做的是同一件事 —— `QPainter::drawPolyline` 直接画点数组 —— 但**不引入
// Qwt**：UI 层刻意只用官方 Qt 模块（见 pipe_client.hpp 顶部那条纪律），
// 而 Qwt 不在两台机器上，也不属于 Qt。

#include <QColor>
#include <QPointF>
#include <QString>
#include <QVector>
#include <QWidget>

namespace libs::ui {

class SpectrumPlot : public QWidget {
public:
    // 一段谱（一台光谱仪的一个波段）。颜色由调用方给 —— 四路四色，
    // 拼接处一眼看得出哪一段是哪台仪器。
    struct Segment {
        QString name;
        QColor color;
        QVector<QPointF> points;   // 原始计数，不归一化
    };

    explicit SpectrumPlot(QWidget* parent = nullptr);

    // 换掉整幅图。内部只存一份点集（move 进来），不做逐点加工。
    void set_segments(QVector<Segment> segments);

    // 置灰：整次采集失败时用。曲线还在，但一眼看出那不是新数据。
    void set_dimmed(bool dimmed);

    void set_auto_y(bool on);
    void set_lock_y(bool on);
    double y_max() const { return y_hi_; }
    void set_y_max(double hi);

    QSize minimumSizeHint() const override { return QSize(480, 260); }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QVector<Segment> segments_;
    bool dimmed_ = false;
    bool auto_y_ = false;
    bool lock_y_ = false;
    double x_lo_ = 185.0;
    double x_hi_ = 565.0;
    double y_lo_ = 0.0;
    double y_hi_ = 64000.0;   // 16 位 ADC 满量程；厂家用的是 -500~66000
};

}  // namespace libs::ui
