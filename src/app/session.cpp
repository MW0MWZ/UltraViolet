// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "app/session.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

#include "app/proms.h"
#include "core/log.h"
#include "net/slirp_backend.h"
#include "net/tap_backend.h"

namespace uv::app {

bool create_blank_image(const std::string& path, u64 bytes, std::string& error) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        error = path + " already exists";
        return false;
    }
    {
        std::ofstream f(path, std::ios::binary);
        if (!f) {
            error = "cannot create " + path;
            return false;
        }
    }
    std::filesystem::resize_file(path, bytes, ec);  // a hole, where the file system has them
    if (ec) {
        error = "cannot size " + path + ": " + ec.message();
        std::filesystem::remove(path, ec);
        return false;
    }
    return true;
}

Session::~Session() {
    if (thread_.joinable()) {
        {
            Hold hold(*this);
            quit_ = true;
        }
        thread_.join();
    }
    power_off();
}

void Session::start_thread() {
    if (!thread_.joinable()) thread_ = std::thread([this] { machine_thread(); });
}

void Session::machine_thread() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        resume_.wait(lock, [this] {
            return quit_ || (holds_.load(std::memory_order_acquire) == 0 && machine_ && !machine_->cpu().halted() &&
                             !machine_->switched_off());
        });
        if (quit_) return;
        machine_->run(slice_);
    }
}

// The pacer waits here while guest time is ahead of the wall clock; a hold
// ends the wait at once (the next slice waits for what is left).
void Session::host_sleep(u64 ns) {
    std::unique_lock<std::mutex> lock(sleep_mutex_);
    wake_.wait_for(lock, std::chrono::nanoseconds(ns), [this] { return holds_.load(std::memory_order_acquire) > 0; });
}

namespace {
thread_local unsigned t_holds = 0;  // holds this thread has
}  // namespace

Session::Hold::Hold(Session& session) : s_(session), nested_(t_holds++ > 0) {
    if (nested_) return;
    s_.holds_.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> lock(s_.sleep_mutex_);
    }
    s_.wake_.notify_all();
    s_.mutex_.lock();
}

Session::Hold::~Hold() {
    --t_holds;
    if (nested_) return;
    // (Released under the lock, so the machine's thread cannot miss it.)
    s_.holds_.fetch_sub(1, std::memory_order_acq_rel);
    s_.mutex_.unlock();
    s_.resume_.notify_all();
}

bool Session::power_on(std::string& error) {
    if (machine_) return true;
    const Settings& s = settings_;
    if (!check_hardware(s, error)) return false;
    PromImage prom;
    if (!select_prom(s, prom, error)) return false;
    apply_log_level(s.log);

    Ultra5_10::Options o = s.options();
    if (!read_prom(prom, o.prom, error)) return false;
    if (s.net == "user") {
        if (net::slirp_available()) o.network = net::make_slirp_backend(s.forwards, error);
        else if (!s.forwards.empty()) error = "port forwarding needs user-mode networking, which this build lacks";
    } else if (s.net.rfind("tap:", 0) == 0) {
        o.network = net::make_tap_backend(s.net.substr(4), error);
    } else if (s.net != "none") {
        error = "network expects user, tap:NAME or none";
    }
    if (!error.empty()) return false;
    if (!o.disk_path.empty() && !std::filesystem::exists(o.disk_path)) {
        error = "the hard disk image " + o.disk_path + " does not exist (Settings > Storage > New... makes one)";
        return false;
    }
    if (turbo_ && turbo_available()) o.timing = Pacer::Mode::Max;
    o.host_clock.sleep_ns = [this](u64 ns) { host_sleep(ns); };
    try {
        machine_ = std::make_unique<Ultra5_10>(o);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    UV_LOG(Info, "session", "flash PROM: %s (OpenBoot %s)", prom.path.c_str(), prom.version.c_str());
    machine_->cpu().set_block_cache(s.block_cache);
    machine_->cpu().set_jit(s.jit);
    machine_->cpu().set_idle_skip(s.idle_skip);
    slice_ = static_cast<u64>(s.cpu_mhz * 1000);
    disc_ = s.cdroms.empty() ? -1 : 0;
    if (disc_ >= 0 && machine_->disc().empty()) {
        UV_LOG(Warn, "session", "cannot open the disc %s", s.cdroms.front().c_str());
        disc_ = -1;
    }
    wire_serial();
    machine_->set_audio_sink(audio_out_);
    return true;
}

void Session::power_off() {
    if (!machine_) return;
    machine_->save_nvram();
    machine_.reset();
    disc_ = -1;
}

std::string Session::stopped() const {
    if (!machine_ || !machine_->cpu().halted()) return {};
    return machine_->cpu().halt_reason().empty() ? std::string("the processor stopped") : machine_->cpu().halt_reason();
}

void Session::set_turbo(bool on) {
    turbo_ = on;
    if (machine_ && turbo_available()) machine_->set_timing(on ? Pacer::Mode::Max : settings_.timing);
}

void Session::set_serial_output(std::function<void(unsigned, u8)> out) {
    serial_out_ = std::move(out);
    if (machine_) wire_serial();
}

void Session::set_audio_output(Cs4231::AudioSink out) {
    audio_out_ = std::move(out);
    if (machine_) machine_->set_audio_sink(audio_out_);
}

void Session::wire_serial() {
    auto out = serial_out_;
    machine_->ttya().transmit = [out](u8 b) {
        if (out) out(0, b);
    };
    machine_->ttyb().transmit = [out](u8 b) {
        if (out) out(1, b);
    };
}

void Session::serial_input(unsigned port, const std::string& bytes) {
    if (!machine_ || bytes.empty()) return;
    SerialLine& line = port == 0 ? machine_->ttya() : machine_->ttyb();
    for (char c : bytes) line.receive(static_cast<u8>(c));
    machine_->poll_serial();
}

void Session::serial_break(unsigned port) {
    if (!machine_) return;
    (port == 0 ? machine_->ttya() : machine_->ttyb()).receive_break();
    machine_->poll_serial();
}

// The drive's eject button does nothing while the system has locked the
// disc in (PREVENT MEDIUM REMOVAL): it has to be ejected from the system.
bool Session::disc_locked() const { return machine_ && !machine_->disc().empty() && machine_->cdrom().locked(); }

bool Session::select_disc(int index) {
    if (!machine_ || index < 0 || index >= static_cast<int>(settings_.cdroms.size())) return false;
    if (disc_locked()) return false;
    if (!machine_->insert_cdrom(settings_.cdroms[static_cast<std::size_t>(index)])) return false;
    disc_ = index;
    return true;
}

bool Session::eject() {
    if (!machine_ || disc_locked()) return false;
    machine_->insert_cdrom({});
    return true;
}

bool Session::next_disc() {
    if (!machine_ || settings_.cdroms.empty()) return false;
    return select_disc(disc_ < 0 ? 0 : (disc_ + 1) % static_cast<int>(settings_.cdroms.size()));
}

bool Session::insert_disc(const std::string& path) {
    if (!machine_) return false;
    auto it = std::find(settings_.cdroms.begin(), settings_.cdroms.end(), path);
    if (it == settings_.cdroms.end()) {
        settings_.cdroms.push_back(path);
        it = settings_.cdroms.end() - 1;
    }
    return select_disc(static_cast<int>(it - settings_.cdroms.begin()));
}

void Session::run(u64 instructions) {
    if (machine_ && !machine_->cpu().halted() && !machine_->switched_off()) machine_->run(instructions);
}

void Session::periodic() {
    if (machine_) machine_->save_nvram();  // (only if the guest changed it)
}

bool Session::save_screenshot(const std::string& path) {
    std::vector<u32> pixels;
    unsigned w = 0, h = 0;
    if (!machine_ || !machine_->graphics().render(pixels, w, h)) return false;
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << w << " " << h << "\n255\n";
    for (u32 p : pixels) {
        const char rgb[3] = {static_cast<char>(p >> 16), static_cast<char>(p >> 8), static_cast<char>(p)};
        f.write(rgb, 3);
    }
    return static_cast<bool>(f);
}

}  // namespace uv::app
