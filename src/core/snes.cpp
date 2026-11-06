// SNES system bus, timing and DMA.
#include "snes.h"
#include "statefix.h"
#include <algorithm>
#include <cstring>

namespace {
const uint8_t kDmaOffs[8][4] = {{0, 0, 0, 0}, {0, 1, 0, 1}, {0, 0, 0, 0}, {0, 0, 1, 1},
                                {0, 1, 2, 3}, {0, 1, 0, 1}, {0, 0, 0, 0}, {0, 0, 1, 1}};
const uint8_t kDmaLen[8] = {1, 2, 2, 4, 4, 4, 2, 4};
constexpr int kVblankLine = 225;
}  // namespace

Snes::Snes() : cpu(this) {
  std::memset(wram, 0, sizeof(wram));
  std::memset(sram, 0, sizeof(sram));
  execMap_.assign(1 << 21, 0);
}

bool Snes::loadRom(const std::vector<uint8_t>& data) {
  size_t off = (data.size() & 0x3ff) == 0x200 ? 0x200 : 0;  // strip copier header
  rom.assign(data.begin() + off, data.end());
  if (rom.empty()) return false;
  size_t sz = 1;
  while (sz < rom.size()) sz <<= 1;
  rom.resize(sz, 0xff);
  romMask_ = sz - 1;
  patchRom.assign(0x80000, 0xff);
  return true;
}

void Snes::markExec(uint32_t pc, bool on) {
  pc &= 0xffffff;
  if (on) execMap_[pc >> 3] |= 1 << (pc & 7);
  else execMap_[pc >> 3] &= ~(1 << (pc & 7));
  anyExecMarks_ = true;
}

void Snes::clearExecMarks() {
  std::fill(execMap_.begin(), execMap_.end(), 0);
  anyExecMarks_ = false;
}

bool Snes::execCheck(uint32_t pc) {
  if (!(execMap_[pc >> 3] & (1 << (pc & 7)))) return false;
  if (!execHook_) return false;
  return execHook_(execCtx_, pc);
}

void Snes::reset(bool hard) {
  cpuCarry_ = 0;
  if (msu) msu->reset();
  if (hard) {
    std::memset(wram, 0x55, sizeof(wram));
    ppu.reset(true);
    apu.reset(true);
  } else {
    ppu.reset(false);
    apu.reset(false);
  }
  cpu.reset(hard);
  cycles_ = 0;
  hPos_ = 0;
  vPos_ = 0;
  lineLen_ = 1364;
  frames_ = 0;
  frameDone_ = false;
  pendingVblank_ = false;
  inVblank_ = false;
  autoJoyTimer_ = 0;
  nmitimen_ = 0;
  nmiFlag_ = irqFlag_ = false;
  htime_ = vtime_ = 0x1ff;
  wrio_ = 0xff;
  mulA_ = 0xff;
  divA_ = 0xffff;
  rdDiv_ = rdMpy_ = 0;
  fastRom_ = false;
  hdmaen_ = 0;
  wmAdr_ = 0;
  std::memset(joyAuto_, 0, sizeof(joyAuto_));
  joyLatch_[0] = joyLatch_[1] = 0;
  joyStrobe_ = false;
  openBus_ = 0;
  for (auto& d : dma_) d = DmaChannel();
  buildLineEvents();
}

// ---------------------------------------------------------------------------
// Timing

int Snes::irqPos() const {
  bool hEn = nmitimen_ & 0x10, vEn = nmitimen_ & 0x20;
  if (!hEn && !vEn) return -1;
  if (hEn) {
    if (htime_ > 339) return -1;
    return htime_ * 4 + 14;
  }
  return 10;
}

void Snes::buildLineEvents() {
  int tmp[8];
  int n = 0;
  int ip = irqPos();
  if (ip >= 0 && ip < lineLen_) tmp[n++] = ip;
  tmp[n++] = 24;    // HDMA init (line 0)
  tmp[n++] = 512;   // render
  tmp[n++] = 536;   // DRAM refresh
  tmp[n++] = 1104;  // HDMA
  std::sort(tmp, tmp + n);
  // de-duplicate
  evCount_ = 0;
  for (int i = 0; i < n; i++) {
    if (evCount_ == 0 || evPos_[evCount_ - 1] != tmp[i]) evPos_[evCount_++] = tmp[i];
  }
  evPos_[evCount_++] = lineLen_;
  evIdx_ = 0;
  while (evIdx_ < evCount_ && evPos_[evIdx_] <= hPos_ && evPos_[evIdx_] != lineLen_) evIdx_++;
}

void Snes::tick(int mc) {
  while (mc > 0) {
    int next = evPos_[evIdx_];
    int step = next - hPos_;
    if (step > mc) step = mc;
    hPos_ += step;
    cycles_ += step;
    mc -= step;
    if (autoJoyTimer_ > 0) autoJoyTimer_ -= step;
    if (hPos_ >= next) {
      int extra = 0;
      if (next == lineLen_) {
        newLine();
      } else {
        evIdx_++;
        processEvent(next, extra);
      }
      mc += extra;
    }
  }
}

void Snes::processEvent(int pos, int& extra) {
  if (pos == irqPos()) checkIrq();
  if (pos == 24 && vPos_ == 0) hdmaInit();
  if (pos == 512) {
    int visible = ppu.overscan() ? 239 : 224;
    if (vPos_ >= 1 && vPos_ <= visible) ppu.renderLine(vPos_);
  }
  if (pos == 536) extra += 40;
  if (pos == 1104 && vPos_ < (ppu.overscan() ? 240 : kVblankLine)) extra += hdmaRun();
}

void Snes::checkIrq() {
  bool hEn = nmitimen_ & 0x10, vEn = nmitimen_ & 0x20;
  if (!hEn && !vEn) return;
  if (vEn && vPos_ != vtime_) return;
  irqFlag_ = true;
  cpu.r.irqWanted = true;
}

void Snes::newLine() {
  hPos_ = 0;
  vPos_++;
  int vblankLine = ppu.overscan() ? 240 : kVblankLine;
  if (vPos_ == vblankLine) {
    pendingVblank_ = true;
    frameDone_ = true;
  }
  if (vPos_ >= 262) {
    vPos_ = 0;
    inVblank_ = false;
    nmiFlag_ = false;
    ppu.frameStart();
    frames_++;
  }
  lineLen_ = 1364;
  buildLineEvents();
}

void Snes::vblankStart() {
  inVblank_ = true;
  nmiFlag_ = true;
  if (nmitimen_ & 0x80) cpu.r.nmiWanted = true;
  ppu.vblankStart();
  if (nmitimen_ & 0x01) {
    joyAuto_[0] = input[0];
    joyAuto_[1] = input[1];
    joyAuto_[2] = joyAuto_[3] = 0;
    autoJoyTimer_ = 4224;
    // After 16 bits are clocked out the serial line reads 1 (controller present).
    joyLatch_[0] = joyLatch_[1] = 0xffff;
  }
}

void Snes::syncApu() {
  uint64_t target = (cycles_ * 15664ull) / 328125ull;
  apu.runTo(target);
}

void Snes::runFrame() {
  frameDone_ = false;
  if (pendingVblank_) {
    pendingVblank_ = false;
    vblankStart();
  }
  // A frame always ends at vblank, except when the overscan bit changes
  // between lines 225 and 240 and neither start line is met: stop then at the
  // second frame start, so the caller still gets one frame at a time.
  uint32_t start = frames_;
  while (!frameDone_ && frames_ - start < 2) {
    cpu.step();
  }
  syncApu();
}

void Snes::stepInstruction() {
  if (pendingVblank_) {
    pendingVblank_ = false;
    vblankStart();
  }
  cpu.step();
}

void Snes::takeAudio(std::vector<int16_t>& out) {
  const size_t at = out.size();
  out.insert(out.end(), apu.samples.begin(), apu.samples.end());
  apu.samples.clear();
  if (msu) msu->mix(out.data() + at, (out.size() - at) / 2, kSampleRate);  // (its music, mixed in)
}

// ---------------------------------------------------------------------------
// Bus

int Snes::accessTime(uint32_t adr) const {
  uint8_t bank = adr >> 16;
  uint16_t a = adr & 0xffff;
  if ((bank & 0x40) == 0) {
    if (a < 0x2000) return 8;
    if (a < 0x4000) return 6;
    if (a < 0x4200) return 12;
    if (a < 0x6000) return 6;
    if (a < 0x8000) return 8;
    return ((bank & 0x80) && fastRom_) ? 6 : 8;
  }
  if (bank >= 0xc0) return fastRom_ ? 6 : 8;
  return 8;
}

uint8_t Snes::cpuRead(uint32_t adr) {
  cpuTick(accessTime(adr));  // (see setCpuSpeed)
  uint8_t v = busRead(adr);
  openBus_ = v;
  if (rwatchFn_) {
    uint32_t n = adr;
    if (((adr >> 16) & 0x40) == 0 && (adr & 0xffff) < 0x2000) n = 0x7e0000 | (adr & 0xffff);
    if (n >= rwatchLo_ && n <= rwatchHi_) rwatchFn_(rwatchCtx_, n, v, cpu.lastPc);
  }
  return v;
}

void Snes::cpuWrite(uint32_t adr, uint8_t v) {
  cpuTick(accessTime(adr));  // (see setCpuSpeed)
  openBus_ = v;
  if (watchFn_ || nWriteHooks_) {
    // Low WRAM mirrors are reported as $7Exxxx
    uint32_t n = adr;
    if (((adr >> 16) & 0x40) == 0 && (adr & 0xffff) < 0x2000) n = 0x7e0000 | (adr & 0xffff);
    if (watchFn_ && n >= watchLo_ && n <= watchHi_) watchFn_(watchCtx_, n, v, cpu.lastPc);
    busWrite(adr, v);
    for (int i = 0; i < nWriteHooks_; i++)
      if (n >= writeHooks_[i].lo && n <= writeHooks_[i].hi) writeHooks_[i].fn(writeHooks_[i].ctx, n, v, cpu.lastPc);
    return;
  }
  busWrite(adr, v);
}

uint8_t Snes::busRead(uint32_t adr) {
  uint8_t bank = adr >> 16;
  uint16_t a = adr & 0xffff;
  if (bank == 0x7e || bank == 0x7f) return wram[((bank & 1) << 16) | a];
  if ((bank & 0x40) == 0) {
    if (a < 0x2000) return wram[a];
    if (a < 0x8000) return readIo(a);
  }
  uint8_t b7 = bank & 0x7f;
  if (a >= 0x8000) {
    if (b7 >= 0x60 && b7 < 0x70) return patchRom[((b7 - 0x60) << 15) | (a & 0x7fff)];
    return rom[(((size_t)b7 << 15) | (a & 0x7fff)) & romMask_];
  }
  if (b7 >= 0x70 && b7 < 0x7e) return sram[a & 0x1fff];
  return rom[(((size_t)b7 << 15) | (a & 0x7fff)) & romMask_];
}

void Snes::busWrite(uint32_t adr, uint8_t v) {
  uint8_t bank = adr >> 16;
  uint16_t a = adr & 0xffff;
  if (bank == 0x7e || bank == 0x7f) {
    wram[((bank & 1) << 16) | a] = v;
    return;
  }
  if ((bank & 0x40) == 0) {
    if (a < 0x2000) {
      wram[a] = v;
      return;
    }
    if (a < 0x8000) {
      writeIo(a, v);
      return;
    }
    return;
  }
  uint8_t b7 = bank & 0x7f;
  if (a < 0x8000 && b7 >= 0x70 && b7 < 0x7e) {
    if (sram[a & 0x1fff] != v) sramDirty = true;
    sram[a & 0x1fff] = v;
  }
}

uint8_t Snes::peek(uint32_t adr) const {
  uint8_t bank = adr >> 16;
  uint16_t a = adr & 0xffff;
  if (bank == 0x7e || bank == 0x7f) return wram[((bank & 1) << 16) | a];
  if ((bank & 0x40) == 0) {
    if (a < 0x2000) return wram[a];
    if (a < 0x8000) return 0;
  }
  uint8_t b7 = bank & 0x7f;
  if (a >= 0x8000) {
    if (b7 >= 0x60 && b7 < 0x70) return patchRom[((b7 - 0x60) << 15) | (a & 0x7fff)];
    return rom[(((size_t)b7 << 15) | (a & 0x7fff)) & romMask_];
  }
  if (b7 >= 0x70 && b7 < 0x7e) return sram[a & 0x1fff];
  return rom[(((size_t)b7 << 15) | (a & 0x7fff)) & romMask_];
}

void Snes::poke(uint32_t adr, uint8_t v) {
  uint8_t bank = adr >> 16;
  uint16_t a = adr & 0xffff;
  if (bank == 0x7e || bank == 0x7f) {
    wram[((bank & 1) << 16) | a] = v;
    return;
  }
  if ((bank & 0x40) == 0 && a < 0x2000) {
    wram[a] = v;
    return;
  }
  uint8_t b7 = bank & 0x7f;
  if (a >= 0x8000 && b7 >= 0x60 && b7 < 0x70) {
    patchRom[((b7 - 0x60) << 15) | (a & 0x7fff)] = v;
    return;
  }
  if (a >= 0x8000) {
    rom[(((size_t)b7 << 15) | (a & 0x7fff)) & romMask_] = v;
    return;
  }
  if (b7 >= 0x70 && b7 < 0x7e) sram[a & 0x1fff] = v;
}

uint8_t Snes::readBBus(uint8_t a) {
  if (a >= 0x34 && a <= 0x3f) return ppu.read(a, openBus_);
  if (a >= 0x40 && a < 0x80) {
    syncApu();
    return apu.cpuReadPort(a & 3);
  }
  if (a == 0x80) {
    uint8_t v = wram[wmAdr_];
    wmAdr_ = (wmAdr_ + 1) & 0x1ffff;
    return v;
  }
  return openBus_;
}

void Snes::writeBBus(uint8_t a, uint8_t v) {
  if (a < 0x34) {
    ppu.write(a, v);
    return;
  }
  if (a >= 0x40 && a < 0x80) {
    syncApu();
    apu.cpuWritePort(a & 3, v);
    if (portFn_) portFn_(portCtx_, a & 3, v);
    return;
  }
  switch (a) {
    case 0x80:
      wram[wmAdr_] = v;
      wmAdr_ = (wmAdr_ + 1) & 0x1ffff;
      break;
    case 0x81: wmAdr_ = (wmAdr_ & 0x1ff00) | v; break;
    case 0x82: wmAdr_ = (wmAdr_ & 0x100ff) | (v << 8); break;
    case 0x83: wmAdr_ = (wmAdr_ & 0x0ffff) | ((v & 1) << 16); break;
    default: break;
  }
}

uint8_t Snes::readIo(uint16_t a) {
  if (msu && a >= 0x2000 && a < 0x2008) return msu->read(a);
  if (a >= 0x2100 && a < 0x2200) {
    if (a == 0x2137) {
      if (wrio_ & 0x80) ppu.latchCounters(hPos_ / 4, vPos_);
      return openBus_;
    }
    return readBBus(a & 0xff);
  }
  if (a == 0x4016 || a == 0x4017) {
    int p = a & 1;
    uint8_t bit = (joyLatch_[p] & 0x8000) ? 1 : 0;
    if (!joyStrobe_) joyLatch_[p] = (joyLatch_[p] << 1) | 1;
    return (openBus_ & 0xfc) | bit | (p ? 0x1c : 0);
  }
  if (a >= 0x4200 && a < 0x4300) {
    switch (a) {
      case 0x4210: {
        uint8_t v = (nmiFlag_ ? 0x80 : 0) | 0x02 | (openBus_ & 0x70);
        nmiFlag_ = false;
        return v;
      }
      case 0x4211: {
        uint8_t v = (irqFlag_ ? 0x80 : 0) | (openBus_ & 0x7f);
        irqFlag_ = false;
        cpu.r.irqWanted = false;
        return v;
      }
      case 0x4212: {
        int vblankLine = ppu.overscan() ? 240 : kVblankLine;
        bool vb = vPos_ >= vblankLine || pendingVblank_;
        bool hb = hPos_ < 4 || hPos_ >= 1096;
        return (vb ? 0x80 : 0) | (hb ? 0x40 : 0) | (autoJoyTimer_ > 0 ? 1 : 0) | (openBus_ & 0x3e);
      }
      case 0x4213: return wrio_;
      case 0x4214: return rdDiv_ & 0xff;
      case 0x4215: return rdDiv_ >> 8;
      case 0x4216: return rdMpy_ & 0xff;
      case 0x4217: return rdMpy_ >> 8;
      case 0x4218: case 0x421a: case 0x421c: case 0x421e: return joyAuto_[(a - 0x4218) >> 1] & 0xff;
      case 0x4219: case 0x421b: case 0x421d: case 0x421f: return joyAuto_[(a - 0x4219) >> 1] >> 8;
      default: return openBus_;
    }
  }
  if (a >= 0x4300 && a < 0x4380) {
    const DmaChannel& d = dma_[(a >> 4) & 7];
    switch (a & 0xf) {
      case 0x0: return d.dmap;
      case 0x1: return d.bbad;
      case 0x2: return d.a1t & 0xff;
      case 0x3: return d.a1t >> 8;
      case 0x4: return d.a1b;
      case 0x5: return d.das & 0xff;
      case 0x6: return d.das >> 8;
      case 0x7: return d.dasb;
      case 0x8: return d.a2a & 0xff;
      case 0x9: return d.a2a >> 8;
      case 0xa: return d.ntrl;
      case 0xb: case 0xf: return d.unused;
      default: return openBus_;
    }
  }
  return openBus_;
}

void Snes::writeIo(uint16_t a, uint8_t v) {
  if (msu && a >= 0x2000 && a < 0x2008) {
    msu->write(a, v);
    return;
  }
  if (a >= 0x2100 && a < 0x2200) {
    writeBBus(a & 0xff, v);
    return;
  }
  if (a == 0x4016) {
    bool strobe = v & 1;
    if (strobe || joyStrobe_) {
      joyLatch_[0] = input[0];
      joyLatch_[1] = input[1];
    }
    joyStrobe_ = strobe;
    return;
  }
  if (a >= 0x4200 && a < 0x4300) {
    switch (a) {
      case 0x4200: {
        bool nmiWas = nmitimen_ & 0x80;
        nmitimen_ = v;
        if (!nmiWas && (v & 0x80) && nmiFlag_) cpu.r.nmiWanted = true;
        if (!(v & 0x30)) {
          irqFlag_ = false;
          cpu.r.irqWanted = false;
        }
        buildLineEvents();
        break;
      }
      case 0x4201:
        if ((wrio_ & 0x80) && !(v & 0x80)) ppu.latchCounters(hPos_ / 4, vPos_);
        wrio_ = v;
        break;
      case 0x4202: mulA_ = v; break;
      case 0x4203:
        rdMpy_ = mulA_ * v;
        rdDiv_ = v;
        break;
      case 0x4204: divA_ = (divA_ & 0xff00) | v; break;
      case 0x4205: divA_ = (divA_ & 0x00ff) | (v << 8); break;
      case 0x4206:
        if (v == 0) {
          rdDiv_ = 0xffff;
          rdMpy_ = divA_;
        } else {
          rdDiv_ = divA_ / v;
          rdMpy_ = divA_ % v;
        }
        break;
      case 0x4207: htime_ = (htime_ & 0x100) | v; buildLineEvents(); break;
      case 0x4208: htime_ = (htime_ & 0x0ff) | ((v & 1) << 8); buildLineEvents(); break;
      case 0x4209: vtime_ = (vtime_ & 0x100) | v; break;
      case 0x420a: vtime_ = (vtime_ & 0x0ff) | ((v & 1) << 8); break;
      case 0x420b: runDma(v); break;
      case 0x420c: hdmaen_ = v; break;
      case 0x420d: fastRom_ = v & 1; break;
      default: break;
    }
    return;
  }
  if (a >= 0x4300 && a < 0x4380) {
    DmaChannel& d = dma_[(a >> 4) & 7];
    switch (a & 0xf) {
      case 0x0: d.dmap = v; break;
      case 0x1: d.bbad = v; break;
      case 0x2: d.a1t = (d.a1t & 0xff00) | v; break;
      case 0x3: d.a1t = (d.a1t & 0x00ff) | (v << 8); break;
      case 0x4: d.a1b = v; break;
      case 0x5: d.das = (d.das & 0xff00) | v; break;
      case 0x6: d.das = (d.das & 0x00ff) | (v << 8); break;
      case 0x7: d.dasb = v; break;
      case 0x8: d.a2a = (d.a2a & 0xff00) | v; break;
      case 0x9: d.a2a = (d.a2a & 0x00ff) | (v << 8); break;
      case 0xa: d.ntrl = v; break;
      case 0xb: case 0xf: d.unused = v; break;
      default: break;
    }
  }
}

// ---------------------------------------------------------------------------
// DMA

uint8_t Snes::dmaReadA(uint32_t adr) {
  uint16_t a = adr & 0xffff;
  uint8_t bank = adr >> 16;
  // A-bus cannot access B-bus / DMA registers
  if ((bank & 0x40) == 0 && ((a >= 0x2100 && a < 0x2200) || (a >= 0x4300 && a < 0x4380) ||
                             a == 0x420b || a == 0x420c))
    return openBus_;
  return busRead(adr);
}

void Snes::dmaWriteA(uint32_t adr, uint8_t v) {
  uint16_t a = adr & 0xffff;
  uint8_t bank = adr >> 16;
  if ((bank & 0x40) == 0 && ((a >= 0x2100 && a < 0x2200) || (a >= 0x4300 && a < 0x4380) ||
                             a == 0x420b || a == 0x420c))
    return;
  busWrite(adr, v);
}

void Snes::runDma(uint8_t mask) {
  if (!mask) return;
  dmaRunning_ = mask;
  tick(8);
  for (int c = 0; c < 8; c++) {
    uint8_t bit = 1 << c;
    if (!(dmaRunning_ & bit)) continue;
    DmaChannel& d = dma_[c];
    tick(8);
    int mode = d.dmap & 7;
    int i = 0;
    do {
      uint32_t aAdr = (uint32_t(d.a1b) << 16) | d.a1t;
      uint8_t bAdr = d.bbad + kDmaOffs[mode][i & 3];
      tick(8);
      if (d.dmap & 0x80) {
        uint8_t v = readBBus(bAdr);
        dmaWriteA(aAdr, v);
      } else {
        uint8_t v = dmaReadA(aAdr);
        writeBBus(bAdr, v);
      }
      if (!(d.dmap & 0x08)) d.a1t += (d.dmap & 0x10) ? -1 : 1;
      i++;
    } while ((dmaRunning_ & bit) && --d.das != 0);
    dmaRunning_ &= ~bit;
  }
  dmaRunning_ = 0;
}

void Snes::hdmaInit() {
  for (int c = 0; c < 8; c++) {
    DmaChannel& d = dma_[c];
    d.doTransfer = false;
    d.terminated = true;
    if (!(hdmaen_ & (1 << c))) continue;
    dmaRunning_ &= ~(1 << c);
    d.a2a = d.a1t;
    d.ntrl = busRead((uint32_t(d.a1b) << 16) | d.a2a++);
    if (d.dmap & 0x40) {
      uint8_t lo = busRead((uint32_t(d.a1b) << 16) | d.a2a++);
      uint8_t hi = busRead((uint32_t(d.a1b) << 16) | d.a2a++);
      d.das = lo | (hi << 8);
    }
    d.terminated = d.ntrl == 0;
    d.doTransfer = true;
  }
}

int Snes::hdmaRun() {
  int cost = 0;
  bool any = false;
  for (int c = 0; c < 8; c++) {
    DmaChannel& d = dma_[c];
    if (!(hdmaen_ & (1 << c)) || d.terminated) continue;
    dmaRunning_ &= ~(1 << c);
    any = true;
    cost += 8;
    if (d.doTransfer) {
      int mode = d.dmap & 7;
      for (int i = 0; i < kDmaLen[mode]; i++) {
        uint32_t src;
        if (d.dmap & 0x40) src = (uint32_t(d.dasb) << 16) | d.das++;
        else src = (uint32_t(d.a1b) << 16) | d.a2a++;
        uint8_t bAdr = d.bbad + kDmaOffs[mode][i];
        if (d.dmap & 0x80) dmaWriteA(src, readBBus(bAdr));
        else writeBBus(bAdr, dmaReadA(src));
        cost += 8;
      }
    }
  }
  for (int c = 0; c < 8; c++) {
    DmaChannel& d = dma_[c];
    if (!(hdmaen_ & (1 << c)) || d.terminated) continue;
    d.ntrl--;
    d.doTransfer = d.ntrl & 0x80;
    if ((d.ntrl & 0x7f) == 0) {
      d.ntrl = busRead((uint32_t(d.a1b) << 16) | d.a2a++);
      if (d.dmap & 0x40) {
        uint8_t lo = busRead((uint32_t(d.a1b) << 16) | d.a2a++);
        uint8_t hi = busRead((uint32_t(d.a1b) << 16) | d.a2a++);
        d.das = lo | (hi << 8);
        cost += 16;
      }
      if (d.ntrl == 0) d.terminated = true;
      d.doTransfer = true;
      cost += 8;
    }
  }
  if (any) cost += 18;
  return cost;
}

// ---------------------------------------------------------------------------
// Save states

namespace {
constexpr uint32_t kStateMagic = 0x53534d53;  // 'SMSS'
// Version 2 appends a CRC-32 of everything before it; version 3 adds the
// game layer's block (StateExt) before the CRC. Older versions still load, and
// every field that indexes memory is clamped either way.
constexpr uint32_t kStateVersion = 3;

uint32_t stateCrc(const uint8_t* d, size_t n) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t v = i;
      for (int k = 0; k < 8; k++) v = (v & 1) ? 0xedb88320u ^ (v >> 1) : v >> 1;
      table[i] = v;
    }
    init = true;
  }
  uint32_t c = ~0u;
  for (size_t i = 0; i < n; i++) c = table[(c ^ d[i]) & 0xff] ^ (c >> 8);
  return ~c;
}

struct Writer {
  std::vector<uint8_t>& out;
  template <typename T>
  void v(const T& x) {
    const uint8_t* p = (const uint8_t*)&x;
    out.insert(out.end(), p, p + sizeof(T));
  }
  void raw(const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    out.insert(out.end(), b, b + n);
  }
};
struct Reader {
  const uint8_t* p;
  const uint8_t* end;
  bool ok = true;
  template <typename T>
  void v(T& x) {
    if (p + sizeof(T) > end) {
      ok = false;
      return;
    }
    std::memcpy(&x, p, sizeof(T));
    p += sizeof(T);
  }
  void raw(void* d, size_t n) {
    if (p + n > end) {
      ok = false;
      return;
    }
    std::memcpy(d, p, n);
    p += n;
  }
};
}  // namespace

std::vector<uint8_t> Snes::saveState() const {
  std::vector<uint8_t> out;
  Writer w{out};
  w.v(kStateMagic);
  w.v(kStateVersion);
  w.v(cpu.r);
  w.raw(wram, sizeof(wram));
  w.raw(sram, sizeof(sram));
  w.v(cycles_);
  w.v(hPos_);
  w.v(vPos_);
  w.v(lineLen_);
  w.v(frames_);
  w.v(pendingVblank_);
  w.v(inVblank_);
  w.v(autoJoyTimer_);
  w.v(nmitimen_);
  w.v(nmiFlag_);
  w.v(irqFlag_);
  w.v(htime_);
  w.v(vtime_);
  w.v(wrio_);
  w.v(mulA_);
  w.v(divA_);
  w.v(rdDiv_);
  w.v(rdMpy_);
  w.v(fastRom_);
  w.v(hdmaen_);
  w.v(wmAdr_);
  w.raw(joyAuto_, sizeof(joyAuto_));
  w.raw(joyLatch_, sizeof(joyLatch_));
  w.v(joyStrobe_);
  w.v(openBus_);
  w.raw(dma_, sizeof(dma_));
  std::vector<uint8_t> p(ppu.stateSize());
  ppu.saveState(p.data());
  uint32_t n = (uint32_t)p.size();
  w.v(n);
  w.raw(p.data(), p.size());
  std::vector<uint8_t> a = apu.saveState();
  n = (uint32_t)a.size();
  w.v(n);
  w.raw(a.data(), a.size());
  std::vector<uint8_t> x;
  if (stateExt) x = stateExt->saveExt();
  n = (uint32_t)x.size();
  w.v(n);
  w.raw(x.data(), x.size());
  // (the faster CPU's cycles carried over, see setCpuSpeed: a tagged byte
  // after the rest, which states without it (and older builds) do without)
  const uint8_t carry[2] = {'K', (uint8_t)cpuCarry_};
  w.raw(carry, 2);
  uint32_t crc = stateCrc(out.data(), out.size());
  w.v(crc);
  return out;
}

bool Snes::loadState(const std::vector<uint8_t>& data) {
  // Sections are applied as they are read; if a later one is rejected, put
  // the machine back exactly as it was.
  std::vector<uint8_t> backup = saveState();
  if (loadStateRaw(data)) return true;
  loadStateRaw(backup);
  return false;
}

bool Snes::loadStateRaw(const std::vector<uint8_t>& data) {
  cpuCarry_ = 0;  // (see setCpuSpeed; kept in the state, else none)
  Reader r{data.data(), data.data() + data.size()};
  uint32_t magic = 0, ver = 0;
  r.v(magic);
  r.v(ver);
  if (!r.ok || magic != kStateMagic || ver < 1 || ver > 3) return false;
  if (ver >= 2) {
    // Damaged file (truncated, bit rot, edited): refuse it outright.
    if (data.size() < 12) return false;
    uint32_t crc;
    std::memcpy(&crc, data.data() + data.size() - 4, 4);
    if (crc != stateCrc(data.data(), data.size() - 4)) return false;
    r.end -= 4;
  }
  r.v(cpu.r);
  r.raw(wram, sizeof(wram));
  r.raw(sram, sizeof(sram));
  r.v(cycles_);
  r.v(hPos_);
  r.v(vPos_);
  r.v(lineLen_);
  r.v(frames_);
  r.v(pendingVblank_);
  r.v(inVblank_);
  r.v(autoJoyTimer_);
  r.v(nmitimen_);
  r.v(nmiFlag_);
  r.v(irqFlag_);
  r.v(htime_);
  r.v(vtime_);
  r.v(wrio_);
  r.v(mulA_);
  r.v(divA_);
  r.v(rdDiv_);
  r.v(rdMpy_);
  r.v(fastRom_);
  r.v(hdmaen_);
  r.v(wmAdr_);
  r.raw(joyAuto_, sizeof(joyAuto_));
  r.raw(joyLatch_, sizeof(joyLatch_));
  r.v(joyStrobe_);
  r.v(openBus_);
  r.raw(dma_, sizeof(dma_));
  uint32_t n = 0;
  r.v(n);
  if (!r.ok || n != ppu.stateSize() || r.p + n > r.end) return false;
  ppu.loadState(r.p);
  r.p += n;
  r.v(n);
  if (!r.ok || r.p + n > r.end) return false;
  if (!apu.loadState(r.p, n)) return false;
  r.p += n;
  const uint8_t* ext = nullptr;
  uint32_t extSize = 0;
  if (ver >= 3) {
    r.v(extSize);
    if (!r.ok || r.p + extSize > r.end) return false;
    ext = r.p;
    r.p += extSize;
  }
  if (r.p + 2 <= r.end && r.p[0] == 'K' && r.p[1] < 4) cpuCarry_ = r.p[1];
  sanitizeState();
  if (stateExt) stateExt->loadExt(ext, extSize);
  frameDone_ = false;
  buildLineEvents();
  sramDirty = true;
  return r.ok;
}

// Values loaded from a state file are only trusted after this: booleans are
// made 0/1 and anything used as an index or address is brought into range.
void Snes::sanitizeState() {
  CpuRegs& c = cpu.r;
  for (bool* b : {&c.c, &c.z, &c.v, &c.n, &c.i, &c.d, &c.xf, &c.mf, &c.e, &c.waiting, &c.stopped, &c.nmiWanted,
                  &c.irqWanted, &c.intPending, &c.resetWanted})
    fixBool(*b);
  for (bool* b : {&pendingVblank_, &inVblank_, &nmiFlag_, &irqFlag_, &fastRom_, &joyStrobe_}) fixBool(*b);
  for (auto& d : dma_) {
    fixBool(d.doTransfer);
    fixBool(d.terminated);
  }
  lineLen_ = 1364;
  vPos_ = vPos_ % 262;
  hPos_ = hPos_ % lineLen_;
  htime_ &= 0x1ff;
  vtime_ &= 0x1ff;
  wmAdr_ &= 0x1ffff;
  autoJoyTimer_ = std::clamp(autoJoyTimer_, 0, 4224);
  // The APU runs to the CPU's cycle count (syncApu). Counters that disagree
  // would have it run billions of cycles to catch up, or stand still until the
  // CPU caught up with it: resynchronise. (The limit keeps the conversion in
  // range: some 300 days of play.)
  if (cycles_ > (1ull << 49)) cycles_ = 0;
  uint64_t apuTarget = (cycles_ * 15664ull) / 328125ull, apuNow = apu.cycles();
  if (apuNow + 100000 < apuTarget || apuNow > apuTarget + 100000) apu.setCycles(apuTarget);
}
