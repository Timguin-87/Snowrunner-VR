#pragma once
#include <d3d11.h>
#include <wrl/client.h>

// What a stretch of GPU work costs, from timestamp queries read back whenever
// they are ready and never waited for, so measuring adds no stall. The answers
// lag the work by a few frames, and a bracket that finds its query slot still
// waiting for one simply goes unmeasured. Immediate context only: that is the
// one that can read the queries back.
class GpuTimer {
public:
    void begin(ID3D11DeviceContext* ctx)
    {
        open_ = false;
        if (!ready(ctx) || !collect(ctx, cursor_)) return;
        ctx->Begin(disjoint_[cursor_].Get());
        ctx->End(start_[cursor_].Get());
        open_ = true;
    }

    void end(ID3D11DeviceContext* ctx)
    {
        if (!open_) return;
        ctx->End(stop_[cursor_].Get());
        ctx->End(disjoint_[cursor_].Get());
        pending_[cursor_] = true;
        cursor_ = (cursor_ + 1) % kSlots;
        open_ = false;
    }

    // The latest measurement in milliseconds; negative until there is one.
    float last_ms() const { return lastMs_; }

    // The mean of everything measured since the last call; negative if nothing was.
    float take_mean_ms()
    {
        const float mean = count_ ? sum_ / (float)count_ : -1.0f;
        sum_ = 0.0f;
        count_ = 0;
        return mean;
    }

private:
    static constexpr int kSlots = 4;

    bool ready(ID3D11DeviceContext* ctx)
    {
        if (ready_) return true;
        if (failed_) return false;
        Microsoft::WRL::ComPtr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        D3D11_QUERY_DESC dj{}; dj.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        D3D11_QUERY_DESC ts{}; ts.Query = D3D11_QUERY_TIMESTAMP;
        for (int i = 0; i < kSlots; ++i) {
            if (!dev || FAILED(dev->CreateQuery(&dj, &disjoint_[i])) ||
                FAILED(dev->CreateQuery(&ts, &start_[i])) ||
                FAILED(dev->CreateQuery(&ts, &stop_[i]))) {
                failed_ = true;
                return false;
            }
        }
        ready_ = true;
        return true;
    }

    // False while this slot's answer is still on its way.
    bool collect(ID3D11DeviceContext* ctx, int slot)
    {
        if (!pending_[slot]) return true;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        UINT64 t0 = 0, t1 = 0;
        if (ctx->GetData(disjoint_[slot].Get(), &dj, sizeof(dj),
                         D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ctx->GetData(start_[slot].Get(), &t0, sizeof(t0),
                         D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ctx->GetData(stop_[slot].Get(), &t1, sizeof(t1),
                         D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            return false;
        pending_[slot] = false;
        if (dj.Disjoint || dj.Frequency == 0 || t1 <= t0) return true;
        lastMs_ = 1000.0f * (float)(t1 - t0) / (float)dj.Frequency;
        sum_ += lastMs_;
        ++count_;
        return true;
    }

    Microsoft::WRL::ComPtr<ID3D11Query> disjoint_[kSlots], start_[kSlots], stop_[kSlots];
    bool  pending_[kSlots] = {};
    int   cursor_ = 0;
    bool  open_ = false, ready_ = false, failed_ = false;
    float lastMs_ = -1.0f, sum_ = 0.0f;
    int   count_ = 0;
};
