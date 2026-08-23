#include "plugin.hpp"
#include <string>
#include <thread>
#include <atomic>
#include <cstring>
#include <dlfcn.h>

// ER-301 headers
extern "C"
{
#include <od/config.h>
#include <hal/constants.h>
#include <hal/channels.h>
#include <hal/pump.h>
#include <hal/audio.h>
#include <hal/heap.h>
#include <hal/timing.h>
#include <hal/uart.h>
#include <hal/card.h>
#include <hal/log.h>
#include <hal/gpio.h>
#include <hal/events.h>
#include <hal/usb.h>
#include <hal/encoder.h>
#include <hal/pwm.h>
#include <hal/adc.h>
#include <hal/modulation.h>
#include <hal/display.h>
#include <hal/rng.h>
#include <emu/tls.h>

  // Custom PWM readback for VCV lights
  void Pwm_getLed(int channel, float *red, float *green);
}

#include <od/glue/AppInterpreter.h>
#include <od/extras/Random.h>
#include <od/AudioThread.h>

struct ER301Module : Module
{
  // ── Enum IDs matching SVG panel component labels ──
  enum InputId
  {
    G1_INPUT,
    IN1_INPUT,
    G2_INPUT,
    IN2_INPUT,
    G3_INPUT,
    IN3_INPUT,
    G4_INPUT,
    IN4_INPUT,
    A1_INPUT,
    B1_INPUT,
    C1_INPUT,
    D1_INPUT,
    A2_INPUT,
    B2_INPUT,
    C2_INPUT,
    D2_INPUT,
    A3_INPUT,
    B3_INPUT,
    C3_INPUT,
    D3_INPUT,
    NUM_INPUTS
  };

  enum OutputId
  {
    OUT1_OUTPUT,
    OUT2_OUTPUT,
    OUT3_OUTPUT,
    OUT4_OUTPUT,
    NUM_OUTPUTS
  };

  enum ParamId
  {
    NUM_PARAMS
  };

  enum LightId
  {
    // Output channel LEDs (GPIO, single color)
    LED_1_LIGHT,
    LED_2_LIGHT,
    LED_3_LIGHT,
    LED_4_LIGHT,
    // Link LEDs (GPIO, single color)
    LINKED1_2_LIGHT,
    LINKED2_3_LIGHT,
    LINKED3_4_LIGHT,
    // Dial LEDs (GPIO, single color)
    LED_FINE_LIGHT,
    LED_COARSE_LIGHT,
    // Status LEDs (GPIO, single color)
    LED_I_O_LIGHT,
    LED_SAFE_LIGHT,
    // CV input LEDs (PWM, bicolor green+red — 2 IDs each)
    LED_A1_GREEN, LED_A1_RED,
    LED_A2_GREEN, LED_A2_RED,
    LED_A3_GREEN, LED_A3_RED,
    LED_B1_GREEN, LED_B1_RED,
    LED_B2_GREEN, LED_B2_RED,
    LED_B3_GREEN, LED_B3_RED,
    LED_C1_GREEN, LED_C1_RED,
    LED_C2_GREEN, LED_C2_RED,
    LED_C3_GREEN, LED_C3_RED,
    LED_D1_GREEN, LED_D1_RED,
    LED_D2_GREEN, LED_D2_RED,
    LED_D3_GREEN, LED_D3_RED,
    NUM_LIGHTS
  };

  // Audio frame buffers
  float inFrame[MAX_AUDIO_FRAME_LENGTH * NUM_INPUT_CHANNELS];
  float outFrame[MAX_AUDIO_FRAME_LENGTH * NUM_OUTPUT_CHANNELS];
  int framePos = 0;
  int outputReadPos = 0;

  // Map from VCV input port index to ER-301 channel index in the interleaved frame
  int inputChannelMap[NUM_INPUTS];

  // Engine state
  std::atomic<bool> engineReady{false};
  std::atomic<bool> audioReady{false};
  std::atomic<bool> engineFailed{false};
  std::string engineError;
  std::string sampleRateWarning;
  std::thread luaThread;
  bool initialized = false;

  // Persistent path strings (Config_init stores raw pointers)
  std::string xRootStr;
  std::string rearRootStr;
  std::string frontRootStr;
  std::string firmwareCfgStr;

  static std::atomic<int> instanceCount;

  ER301Module()
  {
    instanceCount.fetch_add(1, std::memory_order_relaxed);
    config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);

    configInput(G1_INPUT, "G1 Gate");
    configInput(G2_INPUT, "G2 Gate");
    configInput(G3_INPUT, "G3 Gate");
    configInput(G4_INPUT, "G4 Gate");
    configInput(IN1_INPUT, "IN1 Audio");
    configInput(IN2_INPUT, "IN2 Audio");
    configInput(IN3_INPUT, "IN3 Audio");
    configInput(IN4_INPUT, "IN4 Audio");
    configInput(A1_INPUT, "A1 CV");
    configInput(A2_INPUT, "A2 CV");
    configInput(A3_INPUT, "A3 CV");
    configInput(B1_INPUT, "B1 CV");
    configInput(B2_INPUT, "B2 CV");
    configInput(B3_INPUT, "B3 CV");
    configInput(C1_INPUT, "C1 CV");
    configInput(C2_INPUT, "C2 CV");
    configInput(C3_INPUT, "C3 CV");
    configInput(D1_INPUT, "D1 CV");
    configInput(D2_INPUT, "D2 CV");
    configInput(D3_INPUT, "D3 CV");

    configOutput(OUT1_OUTPUT, "OUT1");
    configOutput(OUT2_OUTPUT, "OUT2");
    configOutput(OUT3_OUTPUT, "OUT3");
    configOutput(OUT4_OUTPUT, "OUT4");

    // Map VCV port indices -> ER-301 interleaved channel indices
    inputChannelMap[IN1_INPUT] = INPUT_IN1;
    inputChannelMap[IN2_INPUT] = INPUT_IN2;
    inputChannelMap[IN3_INPUT] = INPUT_IN3;
    inputChannelMap[IN4_INPUT] = INPUT_IN4;
    inputChannelMap[A1_INPUT] = INPUT_A1;
    inputChannelMap[B1_INPUT] = INPUT_B1;
    inputChannelMap[C1_INPUT] = INPUT_C1;
    inputChannelMap[D1_INPUT] = INPUT_D1;
    inputChannelMap[A2_INPUT] = INPUT_A2;
    inputChannelMap[B2_INPUT] = INPUT_B2;
    inputChannelMap[C2_INPUT] = INPUT_C2;
    inputChannelMap[D2_INPUT] = INPUT_D2;
    inputChannelMap[A3_INPUT] = INPUT_A3;
    inputChannelMap[B3_INPUT] = INPUT_B3;
    inputChannelMap[C3_INPUT] = INPUT_C3;
    inputChannelMap[D3_INPUT] = INPUT_D3;
    inputChannelMap[G1_INPUT] = INPUT_G1;
    inputChannelMap[G2_INPUT] = INPUT_G2;
    inputChannelMap[G3_INPUT] = INPUT_G3;
    inputChannelMap[G4_INPUT] = INPUT_G4;

    memset(inFrame, 0, sizeof(inFrame));
    memset(outFrame, 0, sizeof(outFrame));
  }

  ~ER301Module()
  {
    if (initialized)
    {
      Events_push(EVENT_QUIT);
      if (luaThread.joinable())
      {
        luaThread.join();
      }
      Audio_stop();
    }
    instanceCount.fetch_sub(1, std::memory_order_relaxed);
  }

  void initEngine()
  {
    if (initialized)
      return;
    initialized = true;

    // Only one ER-301 instance can run (engine uses global state)
    if (instanceCount.load(std::memory_order_relaxed) > 1)
    {
      engineError = "Only one ER-301 instance is supported. Delete this module and keep the other one.";
      engineFailed.store(true, std::memory_order_release);
      WARN("ER-301: %s", engineError.c_str());
      return;
    }

    TLS_setName("main");

    // Create directories — store as member variables so pointers stay valid.
    // Only the REAR card is isolated (~/.od-vcv): its DSP packages must be built
    // with clang/libc++ to match this plugin's std::string ABI, whereas the SDL
    // emulator's ~/.od packages are GCC. The FRONT card (samples, recordings,
    // quicksaves, browser history) has no ABI-specific content, so we SHARE the
    // emulator's ~/.od/front — that's where the user's samples live, and its
    // persisted browser paths resolve correctly there.
    std::string homeDir = getenv("HOME");
    rearRootStr = homeDir + "/.od-vcv/rear";
    frontRootStr = homeDir + "/.od/front";

    // Find xroot — bundled with plugin or from er-301 source
    std::string pluginPath = rack::asset::plugin(pluginInstance, "res/xroot");
    if (rack::system::isDirectory(pluginPath))
    {
      xRootStr = pluginPath;
    }
    else
    {
      // Fallback: use er-301 source tree xroot
      xRootStr = rack::asset::plugin(pluginInstance, "er-301/xroot");
    }

    // Ensure directories exist
    rack::system::createDirectories(rearRootStr);
    rack::system::createDirectories(frontRootStr);

    try
    {
      Heap_init();
      Timing_init();
      Uart_init();
      Card_init();
      Uart_enable();
      Log_init();

      logInfo("VCV: xRoot = %s", xRootStr.c_str());
      logInfo("VCV: rearRoot = %s", rearRootStr.c_str());
      logInfo("VCV: frontRoot = %s", frontRootStr.c_str());

      firmwareCfgStr = rearRootStr + "/firmware.cfg";
      Config_init(firmwareCfgStr.c_str(), xRootStr.c_str(), rearRootStr.c_str(), frontRootStr.c_str());

      // Check if VCV rate matches ER-301 rate
      float vcvRate = APP->engine->getSampleRate();
      if (std::abs(vcvRate - (float)globalConfig.sampleRate) > 100.f)
      {
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "%.0fHz != %dHz — restart VCV to apply",
                 vcvRate, globalConfig.sampleRate);
        sampleRateWarning = buf;
        logInfo("VCV: %s", buf);
      }

      Pump_init();
      Rng_init();
      Gpio_init();
      Events_init();
      USB_init();
      Encoder_init();
      Pwm_init();
      Adc_init();
      Modulation_init();
      Audio_init();
      Display_init();
      od::Random::init();
      od::AudioThread::init();
      audioReady.store(true, std::memory_order_release);

      Events_push(EVENT_DISPLAY_READY);

      // Re-open our own dylib with RTLD_GLOBAL so that libcore.so (and other
      // ER-301 mod .so files loaded via Lua's require/dlopen) can resolve
      // symbols exported from the main plugin (e.g. od::ZeroOutput).
      {
        Dl_info info;
        if (dladdr((void *)&Audio_init, &info) && info.dli_fname)
        {
          void *self = dlopen(info.dli_fname, RTLD_NOW | RTLD_GLOBAL);
          if (self)
            logInfo("VCV: Promoted plugin symbols to RTLD_GLOBAL");
          else
            logInfo("VCV: dlopen RTLD_GLOBAL failed: %s", dlerror());
        }
      }
    }
    catch (const std::exception &e)
    {
      engineError = std::string("HAL init failed: ") + e.what();
      engineFailed.store(true, std::memory_order_release);
      WARN("ER-301: %s", engineError.c_str());
      return;
    }
    catch (...)
    {
      engineError = "HAL init failed: unknown error";
      engineFailed.store(true, std::memory_order_release);
      WARN("ER-301: %s", engineError.c_str());
      return;
    }

    logInfo("VCV: Starting Lua interpreter thread...");

    // Start the Lua interpreter thread
    luaThread = std::thread([this]()
                            {
      TLS_setName("lua");
      try
      {
        logInfo("VCV: Lua thread started");
        od::AppInterpreter interp;
        interp.init();
        logInfo("VCV: AppInterpreter initialized");
        interp.execute("package.path = '%s/?.lua;%s/?/init.lua'",
                       globalConfig.xRoot, globalConfig.xRoot);
        interp.execute("app.EMULATION = true");
        interp.execute("app.roots = {x='%s',rear='%s',front='%s'}",
                       globalConfig.xRoot, globalConfig.rearRoot, globalConfig.frontRoot);
        logInfo("VCV: Running logging.lua...");
        interp.execute("dofile('%s/boot/logging.lua')", globalConfig.xRoot);
        logInfo("VCV: Running start.lua...");
        interp.execute("dofile('%s/boot/start.lua')", globalConfig.xRoot);
        logInfo("VCV: start.lua finished");
        engineReady.store(true, std::memory_order_release);
      }
      catch (const std::exception &e)
      {
        engineError = std::string("Lua engine failed: ") + e.what();
        engineFailed.store(true, std::memory_order_release);
        WARN("ER-301: %s", engineError.c_str());
      }
      catch (...)
      {
        engineError = "Lua engine failed: unknown error";
        engineFailed.store(true, std::memory_order_release);
        WARN("ER-301: %s", engineError.c_str());
      } });

    Audio_start();
  }

  void process(const ProcessArgs &args) override
  {
    if (!initialized)
    {
      initEngine();
    }

    // Skip all processing if engine failed
    if (engineFailed.load(std::memory_order_acquire))
      return;

    // Write one sample of input into the interleaved input frame
    int offset = framePos * NUM_INPUT_CHANNELS;
    for (int i = 0; i < NUM_INPUTS; i++)
    {
      float voltage = inputs[i].getVoltage();
      inFrame[offset + inputChannelMap[i]] = voltage / FULLSCALE_IN_VOLTS;
    }

    // Read one sample from the output buffer
    if (outputReadPos < FRAMELENGTH)
    {
      int outOffset = outputReadPos * NUM_OUTPUT_CHANNELS;
      for (int i = 0; i < NUM_OUTPUTS; i++)
      {
        float sample = outFrame[outOffset + i];
        outputs[i].setVoltage(sample * FULLSCALE_IN_VOLTS);
      }
      outputReadPos++;
    }

    framePos++;

    // When we've accumulated a full frame, process it
    if (framePos >= FRAMELENGTH)
    {
      memset(outFrame, 0, sizeof(float) * NUM_OUTPUT_CHANNELS * FRAMELENGTH);

      if (audioReady.load(std::memory_order_acquire))
      {
        Pump_callback(inFrame, outFrame);
        Pump_resetThrottle();
      }

      framePos = 0;
      outputReadPos = 0;
    }

    // Update GPIO-driven LEDs
    lights[LED_FINE_LIGHT].setBrightness(Gpio_read(LED_DIAL1) ? 1.f : 0.f);
    lights[LED_COARSE_LIGHT].setBrightness(Gpio_read(LED_DIAL2) ? 1.f : 0.f);
    lights[LED_I_O_LIGHT].setBrightness(Gpio_read(LED_IO) ? 1.f : 0.f);
    lights[LED_SAFE_LIGHT].setBrightness(Gpio_read(LED_SAFE) ? 1.f : 0.f);
    lights[LED_1_LIGHT].setBrightness(Gpio_read(LED_OUT1) ? 1.f : 0.f);
    lights[LED_2_LIGHT].setBrightness(Gpio_read(LED_OUT2) ? 1.f : 0.f);
    lights[LED_3_LIGHT].setBrightness(Gpio_read(LED_OUT3) ? 1.f : 0.f);
    lights[LED_4_LIGHT].setBrightness(Gpio_read(LED_OUT4) ? 1.f : 0.f);
    lights[LINKED1_2_LIGHT].setBrightness(Gpio_read(LED_LINK12) ? 1.f : 0.f);
    lights[LINKED2_3_LIGHT].setBrightness(Gpio_read(LED_LINK23) ? 1.f : 0.f);
    lights[LINKED3_4_LIGHT].setBrightness(Gpio_read(LED_LINK34) ? 1.f : 0.f);

    // Update PWM-driven CV input LEDs (green/red per channel)
    float pwmR, pwmG;
    // PWM channel order: A1,B1,C1,D1, A2,B2,C2,D2, A3,B3,C3,D3
    static const int cvLightIds[] = {
        LED_A1_GREEN, LED_B1_GREEN, LED_C1_GREEN, LED_D1_GREEN,
        LED_A2_GREEN, LED_B2_GREEN, LED_C2_GREEN, LED_D2_GREEN,
        LED_A3_GREEN, LED_B3_GREEN, LED_C3_GREEN, LED_D3_GREEN};
    for (int i = 0; i < 12; i++)
    {
      Pwm_getLed(i, &pwmR, &pwmG);
      lights[cvLightIds[i]].setBrightness(pwmG);
      lights[cvLightIds[i] + 1].setBrightness(pwmR);
    }
  }
};

// Forward declarations for encoder HAL
void VCV_addEncoderDelta(int delta);

// Forward declarations for the I2C follower HAL (src/hal/i2cSlave.cpp)
bool VCV_i2cPushMessage(uint8_t address, const uint8_t *data, uint8_t length);
bool VCV_i2cIsSlaveOpen(void);
uint8_t VCV_i2cGetOwnAddress(void);

// ─── Button with SVG artwork + GPIO interaction ───
struct ER301Button : SvgWidget
{
  uint32_t gpioId;
  bool pressed = false;

  ER301Button(uint32_t id, const std::string &svgPath)
      : gpioId(id)
  {
    setSvg(Svg::load(svgPath));
  }

  void onButton(const ButtonEvent &e) override
  {
    if (e.button == GLFW_MOUSE_BUTTON_LEFT)
    {
      if (e.action == GLFW_PRESS)
      {
        pressed = true;
        Gpio_write(gpioId, false);
        e.consume(this);
      }
      else if (e.action == GLFW_RELEASE)
      {
        pressed = false;
        Gpio_write(gpioId, true);
        e.consume(this);
      }
    }
  }

  void onDragEnd(const DragEndEvent &e) override
  {
    if (pressed)
    {
      pressed = false;
      Gpio_write(gpioId, true);
    }
  }

  void draw(const DrawArgs &args) override
  {
    SvgWidget::draw(args);
    // Press feedback overlay
    if (pressed)
    {
      NVGcontext *vg = args.vg;
      nvgBeginPath(vg);
      nvgRoundedRect(vg, 0, 0, box.size.x, box.size.y, 2);
      nvgFillColor(vg, nvgRGBA(0, 0, 0, 60));
      nvgFill(vg);
    }
  }
};

// Helper to create a button at a position (top-left)
static ER301Button *createER301Button(uint32_t gpioId, Vec pos, const std::string &svgFile)
{
  ER301Button *btn = new ER301Button(gpioId, asset::plugin(pluginInstance, svgFile));
  btn->box.pos = pos;
  return btn;
}

// ─── Toggle switch (3-position) with SVG frames + GPIO interaction ───
struct ER301Toggle : OpaqueWidget
{
  uint32_t idA, idB;
  int state = 1; // 0=up, 1=middle, 2=down
  std::shared_ptr<window::Svg> frames[3];
  SvgWidget *sw;

  float toggleScale = 1.f;

  ER301Toggle(uint32_t a, uint32_t b)
      : idA(a), idB(b)
  {
    sw = new SvgWidget;
    addChild(sw);
    frames[0] = Svg::load(asset::plugin(pluginInstance, "res/components/NKK_2.svg"));
    frames[1] = Svg::load(asset::plugin(pluginInstance, "res/components/NKK_1.svg"));
    frames[2] = Svg::load(asset::plugin(pluginInstance, "res/components/NKK_0.svg"));
    sw->setSvg(frames[1]);
    // Scale down to match panel placeholder (~5.5mm wide)
    float targetW = mm2px(Vec(5.5f, 0)).x;
    toggleScale = targetW / sw->box.size.x;
    box.size = sw->box.size.mult(toggleScale);
    setState(1);
  }

  void setState(int s)
  {
    state = s;
    if (sw && frames[s])
    {
      sw->setSvg(frames[s]);
    }
    switch (s)
    {
    case 0:
      Gpio_write(idA, true);
      Gpio_write(idB, false);
      break;
    case 1:
      Gpio_write(idA, false);
      Gpio_write(idB, false);
      break;
    case 2:
      Gpio_write(idA, false);
      Gpio_write(idB, true);
      break;
    }
  }

  void onButton(const ButtonEvent &e) override
  {
    if (e.button == GLFW_MOUSE_BUTTON_LEFT && e.action == GLFW_PRESS)
    {
      float p = e.pos.y / box.size.y;
      int target = (p < 0.333f) ? 0 : (p < 0.667f) ? 1
                                                     : 2;
      setState(target);
      e.consume(this);
    }
  }

  void draw(const DrawArgs &args) override
  {
    NVGcontext *vg = args.vg;
    nvgSave(vg);
    nvgScale(vg, toggleScale, toggleScale);
    OpaqueWidget::draw(args);
    nvgRestore(vg);
  }
};

// ─── Encoder knob with SVG artwork + drag/scroll interaction ───
struct ER301Knob : SvgWidget
{
  bool dragging = false;
  float svgScale = 1.f;
  float angle = 0.f;

  ER301Knob()
  {
    setSvg(Svg::load(asset::plugin(pluginInstance, "res/components/Rogan2SGray.svg")));
    // Scale to match placeholder (slightly smaller than 29.5mm)
    float targetSize = mm2px(Vec(25.0f, 0)).x;
    svgScale = targetSize / box.size.x;
    box.size = box.size.mult(svgScale);
  }

  void draw(const DrawArgs &args) override
  {
    NVGcontext *vg = args.vg;
    float cx = box.size.x / 2;
    float cy = box.size.y / 2;
    nvgSave(vg);
    nvgTranslate(vg, cx, cy);
    nvgRotate(vg, angle);
    nvgScale(vg, svgScale, svgScale);
    nvgTranslate(vg, -cx / svgScale, -cy / svgScale);
    SvgWidget::draw(args);
    nvgRestore(vg);
  }

  void onButton(const ButtonEvent &e) override
  {
    if (e.button == GLFW_MOUSE_BUTTON_LEFT)
    {
      if (e.action == GLFW_PRESS)
      {
        dragging = true;
        e.consume(this);
      }
      else if (e.action == GLFW_RELEASE)
      {
        dragging = false;
        e.consume(this);
      }
    }
  }

  void onDragMove(const DragMoveEvent &e) override
  {
    float delta = e.mouseDelta.y * 0.5f;
    VCV_addEncoderDelta((int)(delta * 1));
    angle += delta * 0.02f;
  }

  void onHover(const HoverEvent &e) override
  {
    e.consume(this);
  }

  void onHoverScroll(const HoverScrollEvent &e) override
  {
    float delta = -e.scrollDelta.y;
    VCV_addEncoderDelta((int)(delta * 0.12f));
    angle += delta * 0.02f;
    e.consume(this);
  }
};

// ─── Keyboard capture layer ───
// A transparent, focusable widget over the main display. Clicking it (or the
// context-menu action) grabs keyboard focus, after which physical key presses
// drive the ER-301 controls through the same GPIO/encoder path the mouse uses.
// The keymap mirrors the SDL emulator (emu/Emulator.cpp loadDefaultConfiguration).
// Focus/consume approach follows monome-rack's TeletypeScreenWidget.
struct ER301KeyboardCapture : OpaqueWidget
{
  ER301Module *module = nullptr;
  ER301Toggle *storageToggle = nullptr;
  ER301Toggle *modeToggle = nullptr;
  bool storageFocus = false; // Z held: arrows drive the STORAGE toggle
  bool modeFocus = false;    // X held: arrows drive the MODE toggle

  // Emulator encoder tuning (constants.h ENCODER_SPEED = 5): Left/Right are the
  // coarse jump, Up/Down the fine single step (integerised from the emu's 5 / 1.25).
  static constexpr int ENC_COARSE = 5;
  static constexpr int ENC_FINE = 1;

  // GLFW key (US-QWERTY physical position, matching the emulator) → GPIO button id.
  static int buttonForKey(int key)
  {
    switch (key)
    {
    case GLFW_KEY_Q: return BUTTON_MAIN1;
    case GLFW_KEY_W: return BUTTON_MAIN2;
    case GLFW_KEY_E: return BUTTON_MAIN3;
    case GLFW_KEY_R: return BUTTON_MAIN4;
    case GLFW_KEY_T: return BUTTON_MAIN5;
    case GLFW_KEY_Y: return BUTTON_MAIN6;
    case GLFW_KEY_A: return BUTTON_DIAL1;
    case GLFW_KEY_S: return BUTTON_DIAL2;
    case GLFW_KEY_D: return BUTTON_DIAL3;
    case GLFW_KEY_F: return BUTTON_SUB1;
    case GLFW_KEY_G: return BUTTON_SUB2;
    case GLFW_KEY_H: return BUTTON_SUB3;
    case GLFW_KEY_V: return BUTTON_ENTER;
    case GLFW_KEY_B: return BUTTON_UP;
    case GLFW_KEY_N: return BUTTON_SHIFT;
    case GLFW_KEY_1: return BUTTON_SELECT1;
    case GLFW_KEY_2: return BUTTON_SELECT2;
    case GLFW_KEY_3: return BUTTON_SELECT3;
    case GLFW_KEY_4: return BUTTON_SELECT4;
    default: return -1;
    }
  }

  // Release every mappable button (idempotent — Gpio_write only emits on a
  // transition). Called on deselect so a key held while focus is lost can't
  // leave a button stuck LOW/pressed.
  void releaseAllButtons()
  {
    static const int ids[] = {
        BUTTON_MAIN1, BUTTON_MAIN2, BUTTON_MAIN3, BUTTON_MAIN4, BUTTON_MAIN5, BUTTON_MAIN6,
        BUTTON_DIAL1, BUTTON_DIAL2, BUTTON_DIAL3, BUTTON_SUB1, BUTTON_SUB2, BUTTON_SUB3,
        BUTTON_ENTER, BUTTON_UP, BUTTON_SHIFT,
        BUTTON_SELECT1, BUTTON_SELECT2, BUTTON_SELECT3, BUTTON_SELECT4};
    for (int id : ids)
      Gpio_write(id, true);
  }

  // Step a 3-position toggle one notch toward "up" (dir<0) or "down" (dir>0),
  // mirroring the emulator's switchUp/switchDown (end → center → other end).
  void stepToggle(ER301Toggle *t, int dir)
  {
    if (!t)
      return;
    int s = t->state + (dir > 0 ? 1 : -1);
    if (s < 0)
      s = 0;
    if (s > 2)
      s = 2;
    t->setState(s);
  }

  void onButton(const ButtonEvent &e) override
  {
    if (e.button == GLFW_MOUSE_BUTTON_LEFT && e.action == GLFW_PRESS)
    {
      APP->event->setSelectedWidget(this); // grab keyboard focus
      e.consume(this);
      return;
    }
    OpaqueWidget::onButton(e);
  }

  void onDeselect(const DeselectEvent &e) override
  {
    releaseAllButtons();
    storageFocus = false;
    modeFocus = false;
    OpaqueWidget::onDeselect(e);
  }

  void onSelectKey(const SelectKeyEvent &e) override
  {
    if (!module)
    {
      OpaqueWidget::onSelectKey(e);
      return;
    }

    const bool down = (e.action == GLFW_PRESS);
    const bool up = (e.action == GLFW_RELEASE);
    const bool repeat = (e.action == GLFW_REPEAT);

    // Toggle-focus modifiers, held like the emulator's Z / X.
    if (e.key == GLFW_KEY_Z)
    {
      if (down)
        storageFocus = true;
      else if (up)
        storageFocus = false;
      e.consume(this);
      return;
    }
    if (e.key == GLFW_KEY_X)
    {
      if (down)
        modeFocus = true;
      else if (up)
        modeFocus = false;
      e.consume(this);
      return;
    }

    // Arrow keys: encoder, or a focused toggle when Z/X is held.
    if (e.key == GLFW_KEY_LEFT || e.key == GLFW_KEY_RIGHT ||
        e.key == GLFW_KEY_UP || e.key == GLFW_KEY_DOWN)
    {
      if (down || repeat)
      {
        switch (e.key)
        {
        case GLFW_KEY_RIGHT:
          VCV_addEncoderDelta(+ENC_COARSE);
          break;
        case GLFW_KEY_LEFT:
          VCV_addEncoderDelta(-ENC_COARSE);
          break;
        case GLFW_KEY_UP:
          if (storageFocus)
            stepToggle(storageToggle, -1);
          else if (modeFocus)
            stepToggle(modeToggle, -1);
          else
            VCV_addEncoderDelta(+ENC_FINE);
          break;
        case GLFW_KEY_DOWN:
          if (storageFocus)
            stepToggle(storageToggle, +1);
          else if (modeFocus)
            stepToggle(modeToggle, +1);
          else
            VCV_addEncoderDelta(-ENC_FINE);
          break;
        }
      }
      e.consume(this);
      return;
    }

    // Buttons: press on key-down, release on key-up. REPEAT is ignored — the
    // button is already held LOW and the engine runs its own auto-repeat.
    int id = buttonForKey(e.key);
    if (id >= 0)
    {
      if (down)
        Gpio_write(id, false);
      else if (up)
        Gpio_write(id, true);
      e.consume(this);
      return;
    }

    // Swallow all other keys while focused so Rack's global shortcuts don't fire.
    e.consume(this);
  }
};

struct ER301Widget : ModuleWidget
{
  int mainImage = -1;
  int subImage = -1;
  ER301KeyboardCapture *keyboardCapture = nullptr;
  static constexpr int UPSCALE = 4;
  uint8_t mainPixels[MAIN_HORIZONTAL_PIXELS * UPSCALE * MAIN_VERTICAL_PIXELS * UPSCALE * 4];
  uint8_t subPixels[SUB_HORIZONTAL_PIXELS * UPSCALE * SUB_VERTICAL_PIXELS * UPSCALE * 4];

  static constexpr int SCREEN_BRIGHTNESS = 15;
  static constexpr float SCREEN_TINT = 0.85f;

  // Display positions (computed from SVG in constructor, stored in px)
  float mainDispX, mainDispY, mainDispW, mainDispH;
  float subDispX, subDispY, subDispW, subDispH;

  ER301Widget(ER301Module *module)
  {
    setModule(module);
    memset(mainPixels, 0, sizeof(mainPixels));
    memset(subPixels, 0, sizeof(subPixels));

    // ── SVG Panel ──
    setPanel(createPanel(asset::plugin(pluginInstance, "res/ER301.svg")));

    // ── Display positions from SVG (Screen-Large and Screen-small) ──
    Vec mainPos = mm2px(Vec(5.743f, 15.814f));
    Vec mainSize = mm2px(Vec(78.493f, 20.270f));
    mainDispX = mainPos.x;
    mainDispY = mainPos.y;
    mainDispW = mainSize.x;
    mainDispH = mainSize.y;

    Vec subPos = mm2px(Vec(48.280f, 64.859f));
    Vec subSize = mm2px(Vec(36.372f, 18.502f));
    subDispX = subPos.x;
    subDispY = subPos.y;
    subDispW = subSize.x;
    subDispH = subSize.y;

    // ── Screws ──
    addChild(createWidgetCentered<ScrewSilver>(mm2px(Vec(7.656f, 2.688f))));
    addChild(createWidgetCentered<ScrewSilver>(mm2px(Vec(144.791f, 2.688f))));
    addChild(createWidgetCentered<ScrewSilver>(mm2px(Vec(7.656f, 125.691f))));
    addChild(createWidgetCentered<ScrewSilver>(mm2px(Vec(144.791f, 125.691f))));

    // ── Input jacks ──
    addInput(createInputCentered<PJ301MPort>(mm2px(Vec(116.806f, 17.982f)), module, ER301Module::G1_INPUT));
    addInput(createInputCentered<PJ301MPort>(mm2px(Vec(131.257f, 17.982f)), module, ER301Module::IN1_INPUT));
    addInput(createInputCentered<PJ301MPort>(mm2px(Vec(116.806f, 34.161f)), module, ER301Module::G2_INPUT));
    addInput(createInputCentered<PJ301MPort>(mm2px(Vec(131.257f, 34.161f)), module, ER301Module::IN2_INPUT));
    addInput(createInputCentered<PJ301MPort>(mm2px(Vec(116.806f, 50.185f)), module, ER301Module::G3_INPUT));
    addInput(createInputCentered<PJ301MPort>(mm2px(Vec(131.257f, 50.185f)), module, ER301Module::IN3_INPUT));
    addInput(createInputCentered<PJ301MPort>(mm2px(Vec(116.806f, 66.018f)), module, ER301Module::G4_INPUT));
    addInput(createInputCentered<PJ301MPort>(mm2px(Vec(131.257f, 66.018f)), module, ER301Module::IN4_INPUT));
    addInput(createInputCentered<PJ301MPort>(mm2px(Vec(102.295f, 82.273f)), module, ER301Module::A1_INPUT));
    addInput(createInputCentered<PJ301MPort>(mm2px(Vec(116.806f, 82.273f)), module, ER301Module::B1_INPUT));
    addInput(createInputCentered<PJ301MPort>(mm2px(Vec(131.257f, 82.273f)), module, ER301Module::C1_INPUT));
    addInput(createInputCentered<PJ301MPort>(mm2px(Vec(145.572f, 82.273f)), module, ER301Module::D1_INPUT));
    addInput(createInputCentered<PJ301MPort>(mm2px(Vec(102.295f, 98.244f)), module, ER301Module::A2_INPUT));
    addInput(createInputCentered<PJ301MPort>(mm2px(Vec(116.806f, 98.244f)), module, ER301Module::B2_INPUT));
    addInput(createInputCentered<PJ301MPort>(mm2px(Vec(131.257f, 98.244f)), module, ER301Module::C2_INPUT));
    addInput(createInputCentered<PJ301MPort>(mm2px(Vec(145.572f, 98.244f)), module, ER301Module::D2_INPUT));
    addInput(createInputCentered<PJ301MPort>(mm2px(Vec(102.295f, 114.214f)), module, ER301Module::A3_INPUT));
    addInput(createInputCentered<PJ301MPort>(mm2px(Vec(116.806f, 114.214f)), module, ER301Module::B3_INPUT));
    addInput(createInputCentered<PJ301MPort>(mm2px(Vec(131.257f, 114.214f)), module, ER301Module::C3_INPUT));
    addInput(createInputCentered<PJ301MPort>(mm2px(Vec(145.572f, 114.214f)), module, ER301Module::D3_INPUT));

    // ── Output jacks ──
    addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(145.572f, 17.982f)), module, ER301Module::OUT1_OUTPUT));
    addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(145.572f, 34.161f)), module, ER301Module::OUT2_OUTPUT));
    addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(145.572f, 50.185f)), module, ER301Module::OUT3_OUTPUT));
    addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(145.572f, 66.018f)), module, ER301Module::OUT4_OUTPUT));

    // ── LEDs — Output channel ──
    addChild(createLightCentered<MediumLight<YellowLight>>(mm2px(Vec(92.341f, 18.045f)), module, ER301Module::LED_1_LIGHT));
    addChild(createLightCentered<MediumLight<YellowLight>>(mm2px(Vec(92.425f, 34.161f)), module, ER301Module::LED_2_LIGHT));
    addChild(createLightCentered<MediumLight<YellowLight>>(mm2px(Vec(92.425f, 50.185f)), module, ER301Module::LED_3_LIGHT));
    addChild(createLightCentered<MediumLight<YellowLight>>(mm2px(Vec(92.425f, 66.018f)), module, ER301Module::LED_4_LIGHT));

    // ── LEDs — Link ──
    addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(92.425f, 25.949f)), module, ER301Module::LINKED1_2_LIGHT));
    addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(92.425f, 42.011f)), module, ER301Module::LINKED2_3_LIGHT));
    addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(92.425f, 58.124f)), module, ER301Module::LINKED3_4_LIGHT));

    // ── LEDs — Fine/Coarse ──
    addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(8.793f, 85.234f)), module, ER301Module::LED_FINE_LIGHT));
    addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(14.209f, 89.939f)), module, ER301Module::LED_COARSE_LIGHT));

    // ── LEDs — I/O, Safe ──
    addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(27.042f, 110.552f)), module, ER301Module::LED_I_O_LIGHT));
    addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(27.042f, 116.716f)), module, ER301Module::LED_SAFE_LIGHT));

    // ── LEDs — CV bicolor (green/red) ──
    addChild(createLightCentered<MediumLight<GreenRedLight>>(mm2px(Vec(94.682f, 78.004f)), module, ER301Module::LED_A1_GREEN));
    addChild(createLightCentered<MediumLight<GreenRedLight>>(mm2px(Vec(94.682f, 93.958f)), module, ER301Module::LED_A2_GREEN));
    addChild(createLightCentered<MediumLight<GreenRedLight>>(mm2px(Vec(94.682f, 109.934f)), module, ER301Module::LED_A3_GREEN));
    addChild(createLightCentered<MediumLight<GreenRedLight>>(mm2px(Vec(109.092f, 78.004f)), module, ER301Module::LED_B1_GREEN));
    addChild(createLightCentered<MediumLight<GreenRedLight>>(mm2px(Vec(109.092f, 93.958f)), module, ER301Module::LED_B2_GREEN));
    addChild(createLightCentered<MediumLight<GreenRedLight>>(mm2px(Vec(109.092f, 109.934f)), module, ER301Module::LED_B3_GREEN));
    addChild(createLightCentered<MediumLight<GreenRedLight>>(mm2px(Vec(123.477f, 78.004f)), module, ER301Module::LED_C1_GREEN));
    addChild(createLightCentered<MediumLight<GreenRedLight>>(mm2px(Vec(123.477f, 93.958f)), module, ER301Module::LED_C2_GREEN));
    addChild(createLightCentered<MediumLight<GreenRedLight>>(mm2px(Vec(123.477f, 109.934f)), module, ER301Module::LED_C3_GREEN));
    addChild(createLightCentered<MediumLight<GreenRedLight>>(mm2px(Vec(137.871f, 78.004f)), module, ER301Module::LED_D1_GREEN));
    addChild(createLightCentered<MediumLight<GreenRedLight>>(mm2px(Vec(137.871f, 93.958f)), module, ER301Module::LED_D2_GREEN));
    addChild(createLightCentered<MediumLight<GreenRedLight>>(mm2px(Vec(137.871f, 109.934f)), module, ER301Module::LED_D3_GREEN));

    // ── Buttons — Select 1-4 (grey) ──
    std::string greyBtn = "res/components/GreyButton.svg";
    std::string blueBtn = "res/components/BlueButton.svg";
    addChild(createER301Button(BUTTON_SELECT1, mm2px(Vec(97.995f, 13.921f)), greyBtn));
    addChild(createER301Button(BUTTON_SELECT2, mm2px(Vec(97.995f, 30.100f)), greyBtn));
    addChild(createER301Button(BUTTON_SELECT3, mm2px(Vec(97.995f, 46.124f)), greyBtn));
    addChild(createER301Button(BUTTON_SELECT4, mm2px(Vec(97.995f, 61.956f)), greyBtn));

    // ── Buttons — M1-M6 (grey row) ──
    addChild(createER301Button(BUTTON_MAIN1, mm2px(Vec(4.493f, 46.124f)), greyBtn));
    addChild(createER301Button(BUTTON_MAIN2, mm2px(Vec(19.027f, 46.124f)), greyBtn));
    addChild(createER301Button(BUTTON_MAIN3, mm2px(Vec(33.500f, 46.124f)), greyBtn));
    addChild(createER301Button(BUTTON_MAIN4, mm2px(Vec(47.991f, 46.124f)), greyBtn));
    addChild(createER301Button(BUTTON_MAIN5, mm2px(Vec(62.521f, 46.124f)), greyBtn));
    addChild(createER301Button(BUTTON_MAIN6, mm2px(Vec(76.909f, 46.124f)), greyBtn));

    // ── Buttons — Dial/Sub row ──
    addChild(createER301Button(BUTTON_DIAL1, mm2px(Vec(4.493f, 94.182f)), blueBtn));    // fine/coarse
    addChild(createER301Button(BUTTON_DIAL2, mm2px(Vec(19.027f, 94.182f)), blueBtn));   // cancel
    addChild(createER301Button(BUTTON_DIAL3, mm2px(Vec(33.500f, 94.182f)), blueBtn));   // zero
    addChild(createER301Button(BUTTON_SUB1, mm2px(Vec(47.991f, 94.182f)), greyBtn));    // S1
    addChild(createER301Button(BUTTON_SUB2, mm2px(Vec(62.521f, 94.182f)), greyBtn));    // S2
    addChild(createER301Button(BUTTON_SUB3, mm2px(Vec(76.909f, 94.182f)), greyBtn));    // S3

    // ── Buttons — Hard row (blue: enter, up, shift) ──
    addChild(createER301Button(BUTTON_ENTER, mm2px(Vec(47.991f, 110.153f)), blueBtn));
    addChild(createER301Button(BUTTON_UP, mm2px(Vec(62.521f, 110.153f)), blueBtn));
    addChild(createER301Button(BUTTON_SHIFT, mm2px(Vec(76.909f, 110.153f)), blueBtn));

    // ── Toggle switches (3-position with SVG frames) ──
    ER301Toggle *storageTog = new ER301Toggle(TOGGLE_STORAGE_A, TOGGLE_STORAGE_B);
    storageTog->box.pos = mm2px(Vec(4.5f, 110.5f));
    addChild(storageTog);

    ER301Toggle *modeTog = new ER301Toggle(TOGGLE_MODE_A, TOGGLE_MODE_B);
    modeTog->box.pos = mm2px(Vec(33.0f, 110.5f));
    addChild(modeTog);

    // ── Encoder knob (SVG artwork, centered) ──
    {
      ER301Knob *knob = new ER301Knob();
      Vec knobCenter = mm2px(Vec(23.196f, 72.813f));
      knob->box.pos = Vec(knobCenter.x - knob->box.size.x / 2, knobCenter.y - knob->box.size.y / 2);
      addChild(knob);
    }

    // ── Keyboard capture layer (over the main display) ──
    keyboardCapture = new ER301KeyboardCapture();
    keyboardCapture->module = module;
    keyboardCapture->storageToggle = storageTog;
    keyboardCapture->modeToggle = modeTog;
    keyboardCapture->box.pos = Vec(mainDispX, mainDispY);
    keyboardCapture->box.size = Vec(mainDispW, mainDispH);
    addChild(keyboardCapture);
  }

  ~ER301Widget() {}

  void decodeMainDisplay(DisplayBuffer *buf)
  {
    uint16_t *src = (uint16_t *)buf->main;
    const int W = MAIN_HORIZONTAL_PIXELS * UPSCALE;
    for (int y = 0; y < MAIN_VERTICAL_PIXELS; y++)
    {
      int yy = MAIN_VERTICAL_PIXELS - y - 1;
      for (int x = 0; x < MAIN_HORIZONTAL_PIXELS; x++)
      {
        int xx = MAIN_HORIZONTAL_PIXELS - x - 1;
        uint16_t cell = src[(yy << 7) + (xx >> 1)];
        int shift = ((~xx & 1) << 2);
        int value = (cell >> shift) & 0xF;
        value *= SCREEN_BRIGHTNESS;
        uint8_t r = (uint8_t)std::min(value, 255);
        uint8_t g = (uint8_t)std::min((int)(value * SCREEN_TINT), 255);
        // Write UPSCALE×UPSCALE block
        for (int dy = 0; dy < UPSCALE; dy++)
        {
          for (int dx = 0; dx < UPSCALE; dx++)
          {
            int idx = ((y * UPSCALE + dy) * W + (x * UPSCALE + dx)) * 4;
            mainPixels[idx + 0] = r;
            mainPixels[idx + 1] = g;
            mainPixels[idx + 2] = 0;
            mainPixels[idx + 3] = 255;
          }
        }
      }
    }
  }

  void decodeSubDisplay(DisplayBuffer *buf)
  {
    uint16_t *src = (uint16_t *)buf->sub;
    const int W = SUB_HORIZONTAL_PIXELS * UPSCALE;
    for (int y = 0; y < SUB_VERTICAL_PIXELS; y++)
    {
      int yy = SUB_VERTICAL_PIXELS - y - 1;
      int shift = yy & 0b111;
      for (int x = 0; x < SUB_HORIZONTAL_PIXELS; x++)
      {
        int xx = SUB_HORIZONTAL_PIXELS - x - 1;
        uint16_t cell = src[((yy >> 3) << 7) + xx];
        int value = (cell >> shift) & 1;
        value *= 0xF * SCREEN_BRIGHTNESS;
        uint8_t r = (uint8_t)std::min(value, 255);
        uint8_t g = (uint8_t)std::min((int)(value * SCREEN_TINT), 255);
        // Write UPSCALE×UPSCALE block
        for (int dy = 0; dy < UPSCALE; dy++)
        {
          for (int dx = 0; dx < UPSCALE; dx++)
          {
            int idx = ((y * UPSCALE + dy) * W + (x * UPSCALE + dx)) * 4;
            subPixels[idx + 0] = r;
            subPixels[idx + 1] = g;
            subPixels[idx + 2] = 0;
            subPixels[idx + 3] = 255;
          }
        }
      }
    }
  }

  void draw(const DrawArgs &args) override
  {
    NVGcontext *vg = args.vg;

    // Draw SVG panel and all child widgets first
    ModuleWidget::draw(args);

    ER301Module *mod = dynamic_cast<ER301Module *>(module);

    // ── Error overlay ──
    if (mod && mod->engineFailed.load(std::memory_order_acquire))
    {
      // Red tinted overlay on main display area
      nvgBeginPath(vg);
      nvgRoundedRect(vg, mainDispX, mainDispY, mainDispW, mainDispH, 4.0f);
      nvgFillColor(vg, nvgRGBA(40, 0, 0, 220));
      nvgFill(vg);

      // Error text
      nvgFontSize(vg, 11.0f);
      nvgFillColor(vg, nvgRGBA(255, 60, 60, 255));
      nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
      nvgText(vg, mainDispX + mainDispW / 2, mainDispY + 6, "ER-301 ENGINE ERROR", NULL);

      nvgFontSize(vg, 9.0f);
      nvgFillColor(vg, nvgRGBA(200, 200, 200, 255));
      nvgTextBox(vg, mainDispX + 4, mainDispY + 22, mainDispW - 8,
                 mod->engineError.c_str(), NULL);

      // Also show on sub display
      nvgBeginPath(vg);
      nvgRoundedRect(vg, subDispX, subDispY, subDispW, subDispH, 3.0f);
      nvgFillColor(vg, nvgRGBA(40, 0, 0, 220));
      nvgFill(vg);

      nvgFontSize(vg, 9.0f);
      nvgFillColor(vg, nvgRGBA(255, 60, 60, 255));
      nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
      nvgText(vg, subDispX + subDispW / 2, subDispY + subDispH / 2,
              "See log: ~/.od/er301-vcv.log", NULL);
      return;
    }

    // Trigger display update
    if (mod && mod->audioReady.load(std::memory_order_acquire))
      Events_push(EVENT_DISPLAY_READY);

    // ── Main Display (render pixel buffer on top of SVG display area) ──
    DisplayBuffer *dispBuf = Display_getLastPutBuffer();
    if (dispBuf)
    {
      decodeMainDisplay(dispBuf);
      if (mainImage < 0)
        mainImage = nvgCreateImageRGBA(vg, MAIN_HORIZONTAL_PIXELS * UPSCALE, MAIN_VERTICAL_PIXELS * UPSCALE, NVG_IMAGE_NEAREST, mainPixels);
      else
        nvgUpdateImage(vg, mainImage, mainPixels);
      if (mainImage >= 0)
      {
        NVGpaint paint = nvgImagePattern(vg, mainDispX, mainDispY, mainDispW, mainDispH, 0, mainImage, 1.0f);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, mainDispX, mainDispY, mainDispW, mainDispH, 4.0f);
        nvgFillPaint(vg, paint);
        nvgFill(vg);
      }
    }

    // ── Sub Display ──
    if (dispBuf)
    {
      decodeSubDisplay(dispBuf);
      if (subImage < 0)
        subImage = nvgCreateImageRGBA(vg, SUB_HORIZONTAL_PIXELS * UPSCALE, SUB_VERTICAL_PIXELS * UPSCALE, NVG_IMAGE_NEAREST, subPixels);
      else
        nvgUpdateImage(vg, subImage, subPixels);
      if (subImage >= 0)
      {
        NVGpaint paint = nvgImagePattern(vg, subDispX, subDispY, subDispW, subDispH, 0, subImage, 1.0f);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, subDispX, subDispY, subDispW, subDispH, 3.0f);
        nvgFillPaint(vg, paint);
        nvgFill(vg);
      }
    }

    // ── Sample rate warning ──
    if (mod && !mod->sampleRateWarning.empty())
    {
      nvgFontSize(vg, 8.0f);
      nvgFillColor(vg, nvgRGBA(255, 217, 0, 180));
      nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
      nvgText(vg, mainDispX + mainDispW - 3, mainDispY + 3,
              mod->sampleRateWarning.c_str(), NULL);
    }

    // ── Keyboard-focus ring (drawn on top of the display) ──
    if (keyboardCapture && APP->event->selectedWidget == keyboardCapture)
    {
      nvgBeginPath(vg);
      nvgRoundedRect(vg, mainDispX - 1.5f, mainDispY - 1.5f, mainDispW + 3.0f, mainDispH + 3.0f, 5.0f);
      nvgStrokeColor(vg, nvgRGBA(255, 180, 0, 220));
      nvgStrokeWidth(vg, 1.5f);
      nvgStroke(vg);
    }
  }

  void appendContextMenu(Menu *menu) override
  {
    menu->addChild(new MenuSeparator);
    menu->addChild(createMenuItem("Capture keyboard (or click the display)", "", [this]()
                                  {
      if (keyboardCapture)
        APP->event->setSelectedWidget(keyboardCapture); }));
    menu->addChild(createMenuLabel("Q-Y: main softkeys   A-H: sub buttons"));
    menu->addChild(createMenuLabel("V/B/N: enter/up/shift   1-4: channel select"));
    menu->addChild(createMenuLabel("Arrows: encoder (L/R coarse, U/D fine)"));
    menu->addChild(createMenuLabel("Hold Z/X + arrows: STORAGE/MODE toggles"));

    // ── II / I2C follower debug ──
    //
    // Bring-up harness for the I2C receive path, independent of any leader
    // module: inject a frame exactly as a Teletype would emit it and watch the
    // bound SC unit respond. The teletype package must be loaded and enabled
    // (it opens the follower on 0x31 by default) or these are no-ops.
    menu->addChild(new MenuSeparator);
    if (VCV_i2cIsSlaveOpen())
    {
      menu->addChild(createMenuLabel(
          rack::string::f("II follower listening on 0x%02x", VCV_i2cGetOwnAddress())));

      menu->addChild(createMenuItem("Inject SC.CV 1 5V", "", []()
                                    {
        // 0x10 = SC.CV, port 0 (zero-indexed on the wire), value big-endian
        // int16 at 16384 LSB per volt -> 5V = 0x5000.
        const uint8_t frame[4] = {0x10, 0x00, 0x50, 0x00};
        VCV_i2cPushMessage(VCV_i2cGetOwnAddress(), frame, 4); }));

      menu->addChild(createMenuItem("Inject SC.CV 1 0V", "", []()
                                    {
        const uint8_t frame[4] = {0x10, 0x00, 0x00, 0x00};
        VCV_i2cPushMessage(VCV_i2cGetOwnAddress(), frame, 4); }));

      menu->addChild(createMenuItem("Inject SC.TR.PULSE 1", "", []()
                                    {
        // 0x05 = SC.TR.PULSE, port 0. Value is unused but the decoder wants
        // length > 3 before it will read one.
        const uint8_t frame[4] = {0x05, 0x00, 0x00, 0x00};
        VCV_i2cPushMessage(VCV_i2cGetOwnAddress(), frame, 4); }));
    }
    else
    {
      menu->addChild(createMenuLabel("II follower closed (load the teletype package)"));
    }
  }
};

std::atomic<int> ER301Module::instanceCount{0};

Model *modelER301 = createModel<ER301Module, ER301Widget>("ER301");
