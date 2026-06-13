#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>

class Timer
{
public:
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
    Timer(Timer&&) = delete;
    Timer& operator=(Timer&&) = delete;

    static Timer& GetInstance()
    {
        static Timer instance;
        return instance;
    }

    void Tick()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto now = std::chrono::steady_clock::now();
        std::chrono::duration<float> delta = now - m_lastTimePoint;
        m_deltaTime = delta.count();
        m_totalTime += m_deltaTime * m_timeScale.load();
        m_lastTimePoint = now;
        UpdateFPS();
    }

    float GetDeltaTime() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_deltaTime * m_timeScale.load();
    }

    float GetRawDeltaTime() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_deltaTime;
    }

    float GetTotalTime() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_totalTime;
    }

    void SetTimeScale(float scale)
    {
        m_timeScale.store((std::max)(0.0f, scale));
    }

    int GetFPS() const
    {
        return m_fps.load();
    }

private:
    Timer()
        : m_lastTimePoint(std::chrono::steady_clock::now())
        , m_deltaTime(0.0f)
        , m_totalTime(0.0f)
        , m_timeScale(1.0f)
        , m_fps(0)
        , m_frameCount(0)
        , m_fpsAccumulator(0.0f)
    {
    }

    void UpdateFPS()
    {
        ++m_frameCount;
        m_fpsAccumulator += m_deltaTime;

        if (m_fpsAccumulator >= 1.0f)
        {
            m_fps.store(m_frameCount);
            m_frameCount = 0;
            m_fpsAccumulator -= 1.0f;
        }
    }

    mutable std::mutex m_mutex;
    std::chrono::steady_clock::time_point m_lastTimePoint;
    float m_deltaTime;
    float m_totalTime;
    std::atomic<float> m_timeScale;
    std::atomic<int> m_fps;
    int m_frameCount;
    float m_fpsAccumulator;
};
