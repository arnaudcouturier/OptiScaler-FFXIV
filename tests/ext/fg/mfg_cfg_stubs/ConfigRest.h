// Appended by prepare_dynamic_mfg_config.ps1 to the CustomOptional slice of the real Config.h: just enough of Config for
// FgConfig.cpp (the three upstream members its Bind and PostLoad touch).
enum class LowLatencyInput : uint32_t
{
    None,
    Auto,
    AntiLag2,
    Reflex,
    XeLL,
    UeLowLatency,
    OptiScaler,
    _
};

enum class LowLatencyMode : uint32_t
{
    None,
    Auto,
    LatencyFlex,
    AntiLag2,
    XeLL,
    AntiLagVk,
    Reflex,
    FpsLimiter,
    _
};

class Config
{
  public:
    CustomOptional<int> FGXeFGInterpolationCount { 1 };
    CustomOptional<LowLatencyInput> LowLatencyInput { LowLatencyInput::Auto };
    CustomOptional<LowLatencyMode> LowLatencyOutput { LowLatencyMode::Auto };

    static Config* Instance()
    {
        static Config instance;
        return &instance;
    }
};
