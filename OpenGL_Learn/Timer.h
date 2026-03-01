#pragma once
#include <chrono>
#include <mutex>
#include <atomic>

// Engine Time Manager
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

	// Per-frame update (should be called once per frame)
    void Tick()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto now = std::chrono::steady_clock::now();

		//compute delta time
        std::chrono::duration<float> delta = now - m_lastTimePoint;
        m_deltaTime = delta.count();

		//update total time with time scale
        m_totalTime += m_deltaTime * m_timeScale;

		//update last time point for next frame
        m_lastTimePoint = now;

		//update FPS statistics
        UpdateFPS();
    }

    float GetDeltaTime() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_deltaTime * m_timeScale;
    }

    float GetRawDeltaTime() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_deltaTime;
    }

	// Get total time since engine start (affected by time scale)
    float GetTotalTime() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_totalTime;
    }

	// set time scale (e.g., for slow motion or fast forward)
    void SetTimeScale(float scale)
    {
        m_timeScale = std::max(0.0f, scale);
    }

	//get current fps
    int GetFPS() const
    {
        return m_fps.load(); // atomic无需锁
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
        m_frameCount++;
        m_fpsAccumulator += m_deltaTime;

        if (m_fpsAccumulator >= 1.0f)
        {
            m_fps.store(m_frameCount); // 原子操作，线程安全
            m_frameCount = 0;
            m_fpsAccumulator -= 1.0f; 
        }
    }

    mutable std::mutex m_mutex; // 线程安全锁（mutable允许const函数加锁）
    std::chrono::steady_clock::time_point m_lastTimePoint;

    float m_deltaTime;       
    float m_totalTime;       
    std::atomic<float> m_timeScale; 

    
    std::atomic<int> m_fps;  // 当前帧率（原子变量）
    int m_frameCount;        // 统计周期内的帧数
    float m_fpsAccumulator;  // 帧率统计累计时间
};