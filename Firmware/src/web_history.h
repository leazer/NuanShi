#pragma once

constexpr char webHistoryScript[] = R"HTML(
<script>
const chart = document.getElementById('temperature-chart');
const context = chart.getContext('2d');
const chartNote = document.getElementById('chart-note');
const chartDefaultNote = '北京时间 · 最近 24 小时';
const timeFormat = new Intl.DateTimeFormat('zh-CN', {
    timeZone: 'Asia/Shanghai', hour: '2-digit', minute: '2-digit', hourCycle: 'h23'
});
let historyData = null;
let chartMarkers = [];
let hoveredMarker = null;
function timeLabel(epoch) {
    return timeFormat.format(epoch * 1000);
}
function drawChart() {
    const width = chart.clientWidth;
    const height = chart.clientHeight;
    if (width < 1 || height < 1) return;
    const scale = Math.min(window.devicePixelRatio || 1, 2);
    chart.width = Math.round(width * scale);
    chart.height = Math.round(height * scale);
    context.setTransform(scale, 0, 0, scale, 0, 0);
    context.font = '12px system-ui';
    chartMarkers = [];
    if (!historyData) return;
    const {start, end, rows} = historyData;
    const left = 39;
    const right = width - 36;
    const roomTop = 38;
    const roomBottom = Math.round(height * 0.45);
    const outdoorTop = roomBottom + 27;
    const outdoorBottom = height - 26;
    const xAt = time => left + (time - start) * (right - left) / (end - start);
    const timeTicks = width < 400 ? 4 : 6;
    context.strokeStyle = '#304252';
    context.fillStyle = '#b9c9d6';
    context.textAlign = 'center';
    for (let i = 0; i <= timeTicks; ++i) {
        const time = start + i * (end - start) / timeTicks;
        const x = xAt(time);
        context.beginPath();
        context.moveTo(x, roomTop);
        context.lineTo(x, outdoorBottom);
        context.stroke();
        context.fillText(timeLabel(time), x, height - 8);
    }
    function drawSeries(kind, top, bottom, color, gap) {
        const series = rows.filter(row => row[1] === kind);
        if (!series.length) {
            context.fillStyle = '#b9c9d6';
            context.textAlign = 'center';
            context.fillText('暂无读数', (left + right) / 2, (top + bottom) / 2);
            return;
        }
        const temperatures = series.map(row => row[2]);
        const low = Math.floor(Math.min(...temperatures) - 0.1);
        const high = Math.ceil(Math.max(...temperatures) + 0.1);
        const step = Math.max(1, Math.ceil((high - low) / 4));
        const ceiling = low + Math.ceil((high - low) / step) * step;
        const yAt = value => bottom - (value - low) * (bottom - top) / (ceiling - low);
        context.textAlign = 'right';
        context.fillStyle = '#b9c9d6';
        context.strokeStyle = '#304252';
        for (let temp = low; temp <= ceiling; temp += step) {
            const y = yAt(temp);
            context.fillText(String(temp), left - 5, y + 4);
            context.beginPath();
            context.moveTo(left, y);
            context.lineTo(right, y);
            context.stroke();
        }
        context.strokeStyle = color;
        context.fillStyle = color;
        context.lineWidth = 1.5;
        context.beginPath();
        let previousTime = null;
        for (const [time, , value] of series) {
            const x = xAt(time);
            const y = yAt(value);
            if (previousTime === null || time - previousTime > gap) context.moveTo(x, y);
            else context.lineTo(x, y);
            previousTime = time;
        }
        context.stroke();
        context.lineWidth = 1;
        if (kind === 'outdoor' || series.length === 1) {
            for (const [time, , value] of series) {
                context.beginPath();
                context.arc(xAt(time), yAt(value), 2, 0, 2 * Math.PI);
                context.fill();
            }
        }
    }
    drawSeries('indoor', roomTop, roomBottom, '#79dfc4', 360);
    drawSeries('outdoor', outdoorTop, outdoorBottom, '#93b6ff', 3600);
    const labelEnds = [-Infinity, -Infinity];
    for (const [time, kind, value] of rows) {
        if (kind !== 'gear' && kind !== 'test') continue;
        const x = xAt(time);
        const color = kind === 'test' ? '#c7a8f1' : '#ffc885';
        const label = value.toFixed(1) + ' 档';
        chartMarkers.push({x, time, kind, label});
        context.strokeStyle = color;
        context.setLineDash(kind === 'test' ? [3, 3] : []);
        context.beginPath();
        context.moveTo(x, roomTop);
        context.lineTo(x, outdoorBottom);
        context.stroke();
        context.setLineDash([]);
        const labelWidth = context.measureText(label).width;
        const labelX = Math.max(1, Math.min(width - labelWidth - 1, x - labelWidth / 2));
        const row = labelEnds.findIndex(last => labelX > last + 3);
        if (row >= 0) {
            context.textAlign = 'left';
            context.fillStyle = color;
            context.fillText(label, labelX, 13 + row * 15);
            labelEnds[row] = labelX + labelWidth;
        }
    }
}
function showMarker(event) {
    const x = event.clientX - chart.getBoundingClientRect().left;
    const near = chartMarkers.filter(marker => Math.abs(marker.x - x) < 12);
    hoveredMarker = near[0] || null;
    chartNote.textContent = hoveredMarker ? near.map(marker => timeLabel(marker.time) + ' ' +
        (marker.kind === 'test' ? '测试 ' : '手动 ') + marker.label).join(' · ') : chartDefaultNote;
}
chart.addEventListener('pointermove', showMarker);
chart.addEventListener('pointerdown', showMarker);
chart.addEventListener('pointerleave', () => {
    hoveredMarker = null;
    if (historyData) chartNote.textContent = chartDefaultNote;
});
new ResizeObserver(drawChart).observe(chart);
async function updateChart() {
    if (document.hidden) return;
    try {
        const response = await fetch('/history');
        if (response.status === 401) { location.reload(); return; }
        if (!response.ok) {
            chartNote.textContent = await response.text();
            return;
        }
        const [range, ...lines] = (await response.text()).trim().split('\n');
        const [start, end] = range.split(',').map(Number);
        if (!(end > start)) throw new Error('invalid history range');
        const rows = lines.map(line => {
            const [time, type, value] = line.split(',');
            return [Number(time), type, Number(value)];
        }).sort((a, b) => a[0] - b[0]);
        historyData = {start, end, rows};
        if (!hoveredMarker) chartNote.textContent = rows.length ? chartDefaultNote : '最近 24 小时暂无记录';
        drawChart();
    } catch {
        chartNote.textContent = '温度曲线读取失败，稍后重试。';
    }
}
updateChart();
setInterval(updateChart, 60000);
document.addEventListener('visibilitychange', updateChart);
</script>
)HTML";
