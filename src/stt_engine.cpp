/* Copyright (C) 2023 Michal Kosciesza <michal@mkiol.net>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "stt_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <numeric>
#include <sstream>

#include "logger.hpp"

using namespace std::chrono_literals;

std::ostream& operator<<(std::ostream& os, stt_engine::speech_mode_t mode) {
    switch (mode) {
        case stt_engine::speech_mode_t::automatic:
            os << "automatic";
            break;
        case stt_engine::speech_mode_t::manual:
            os << "manual";
            break;
        case stt_engine::speech_mode_t::single_sentence:
            os << "single-sentence";
            break;
    }

    return os;
}

std::ostream& operator<<(std::ostream& os,
                         stt_engine::speech_detection_status_t status) {
    switch (status) {
        case stt_engine::speech_detection_status_t::no_speech:
            os << "no-speech";
            break;
        case stt_engine::speech_detection_status_t::speech_detected:
            os << "speech-detected";
            break;
        case stt_engine::speech_detection_status_t::decoding:
            os << "decoding";
            break;
        case stt_engine::speech_detection_status_t::initializing:
            os << "initializing";
            break;
    }

    return os;
}

std::ostream& operator<<(std::ostream& os, stt_engine::lock_type_t lock_type) {
    switch (lock_type) {
        case stt_engine::lock_type_t::borrowed:
            os << "borrowed";
            break;
        case stt_engine::lock_type_t::free:
            os << "free";
            break;
        case stt_engine::lock_type_t::processed:
            os << "processed";
            break;
    }

    return os;
}

std::ostream& operator<<(std::ostream& os, stt_engine::flush_t flush_type) {
    switch (flush_type) {
        case stt_engine::flush_t::regular:
            os << "regular";
            break;
        case stt_engine::flush_t::eof:
            os << "eof";
            break;
        case stt_engine::flush_t::restart:
            os << "restart";
            break;
        case stt_engine::flush_t::exit:
            os << "exit";
            break;
    }

    return os;
}

std::ostream& operator<<(std::ostream& os,
                         stt_engine::samples_process_result_t result) {
    switch (result) {
        case stt_engine::samples_process_result_t::no_samples_needed:
            os << "no-samples-needed";
            break;
        case stt_engine::samples_process_result_t::wait_for_samples:
            os << "wait-for-samples";
            break;
    }

    return os;
}

std::ostream& operator<<(std::ostream& os,
                         stt_engine::processing_state_t state) {
    switch (state) {
        case stt_engine::processing_state_t::idle:
            os << "idle";
            break;
        case stt_engine::processing_state_t::initializing:
            os << "initializing";
            break;
        case stt_engine::processing_state_t::decoding:
            os << "decoding";
            break;
    }

    return os;
}

std::ostream& operator<<(std::ostream& os, stt_engine::vad_mode_t mode) {
    switch (mode) {
        case stt_engine::vad_mode_t::aggressiveness0:
            os << "aggressiveness-0";
            break;
        case stt_engine::vad_mode_t::aggressiveness1:
            os << "aggressiveness-1";
            break;
        case stt_engine::vad_mode_t::aggressiveness2:
            os << "aggressiveness-2";
            break;
        case stt_engine::vad_mode_t::aggressiveness3:
            os << "aggressiveness-3";
            break;
    }

    return os;
}

std::ostream& operator<<(std::ostream& os,
                         const stt_engine::model_files_t& model_files) {
    os << "model-file=" << model_files.model_file
       << ", scorer-file=" << model_files.scorer_file
       << ", ttt-model-file=" << model_files.ttt_model_file;

    return os;
}

std::ostream& operator<<(std::ostream& os, const stt_engine::config_t& config) {
    os << "lang=" << config.lang << ", model-files=[" << config.model_files
       << "], speech-mode=" << config.speech_mode
       << ", vad-mode=" << config.vad_mode
       << ", speech-started=" << config.speech_started;

    return os;
}

stt_engine::stt_engine(config_t config, callbacks_t call_backs)
    : m_model_files{std::move(config.model_files)},
      m_lang{std::move(config.lang)},
      m_call_backs{std::move(call_backs)},
      m_speech_started{config.speech_started},
      m_speech_mode{config.speech_mode},
      m_translate{config.translate} {}

stt_engine::~stt_engine() { LOGD("engine dtor"); }

void stt_engine::start() {
    if (started()) {
        LOGW("engine already started");
        return;
    }

    LOGD("starting engine");

    if (m_processing_thread.joinable()) m_processing_thread.join();

    m_thread_exit_requested = false;

    m_processing_thread = std::thread{&stt_engine::start_processing, this};

    LOGD("engine started");
}

bool stt_engine::started() const {
    return m_processing_thread.joinable() && !m_thread_exit_requested;
}

void stt_engine::stop_processing_impl() {}
void stt_engine::start_processing_impl() {}

void stt_engine::stop() {
    if (m_thread_exit_requested) {
        LOGD("engine stop already requested");
        return;
    }

    m_thread_exit_requested = true;

    LOGD("stop requested");

    stop_processing_impl();

    if (!m_processing_thread.joinable()) {
        LOGD("processing thread already stopped");
        return;
    }

    m_processing_cv.notify_all();
    if (m_processing_thread.joinable()) m_processing_thread.join();
    m_speech_started = false;
    set_speech_detection_status(speech_detection_status_t::no_speech);
    set_processing_state(processing_state_t::idle);

    LOGD("stop completed");
}

void stt_engine::start_processing() {
    LOGD("processing started");

    m_thread_exit_requested = false;

    try {
        set_processing_state(processing_state_t::initializing);
        start_processing_impl();
        set_processing_state(processing_state_t::idle);

        while (true) {
            LOGT("processing iter");

            std::unique_lock lock{m_processing_mtx};

            if (m_thread_exit_requested) break;

            if (m_restart_requested) {
                m_restart_requested = false;
                flush(flush_t::restart);
            }

            if (process_buff() == samples_process_result_t::wait_for_samples &&
                !m_thread_exit_requested)
                m_processing_cv.wait(lock);
        }

        flush(flush_t::exit);
    } catch (const std::runtime_error& e) {
        LOGE("processing error: " << e.what());

        if (m_call_backs.error) m_call_backs.error();
    }

    reset_in_processing();

    LOGD("processing ended");
}

bool stt_engine::lock_buf(lock_type_t desired_lock) {
    lock_type_t expected_lock = lock_type_t::free;
    return m_in_buf.lock.compare_exchange_strong(expected_lock, desired_lock);
}

void stt_engine::free_buf(lock_type_t lock) {
    lock_type_t expected_lock = lock;
    m_in_buf.lock.compare_exchange_strong(expected_lock, lock_type_t::free);
}

void stt_engine::free_buf() { m_in_buf.lock.store(lock_type_t::free); }

std::pair<char*, size_t> stt_engine::borrow_buf() {
    decltype(borrow_buf()) c_buf{nullptr, 0};

    if (m_thread_exit_requested) {
        return c_buf;
    }

    if (!lock_buf(lock_type_t::borrowed)) {
        return c_buf;
    }

    if (m_in_buf.full()) {
        LOGD("in-buf is full");
        free_buf();
        return c_buf;
    }

    c_buf.first = reinterpret_cast<char*>(&m_in_buf.buf.at(m_in_buf.size));
    c_buf.second = (m_in_buf.buf.size() - m_in_buf.size) *
                   sizeof(in_buf_t::buf_t::value_type);

    return c_buf;
}

void stt_engine::return_buf(const char* c_buf, size_t size, bool sof,
                            bool eof) {
    if (m_in_buf.lock != lock_type_t::borrowed) return;

    m_in_buf.size =
        (c_buf - reinterpret_cast<char*>(m_in_buf.buf.data()) + size) /
        sizeof(in_buf_t::buf_t::value_type);
    m_in_buf.eof = eof;
    if (sof) m_in_buf.sof = sof;

    free_buf();
    m_processing_cv.notify_one();
}

bool stt_engine::lock_buff_for_processing() {
    if (!lock_buf(lock_type_t::processed)) {
        LOGW("failed to lock for processing, buf is not free");
        return false;
    }

    LOGT("lock buff for processing: eof=" << m_in_buf.eof
                                          << ", buf size=" << m_in_buf.size);

    if (!m_in_buf.eof && m_in_buf.size < m_in_buf_max_size) {
        free_buf();
        return false;
    }

    return true;
}

void stt_engine::reset_in_processing() {
    LOGD("reset in processing");

    m_in_buf.clear();
    m_start_time.reset();
    m_vad.reset();
    m_intermediate_text.reset();
    set_speech_detection_status(speech_detection_status_t::no_speech);

    reset_impl();
}

stt_engine::samples_process_result_t stt_engine::process_buff() {
    return samples_process_result_t::wait_for_samples;
}

std::string stt_engine::merge_texts(const std::string& old_text,
                                    std::string&& new_text) {
    if (new_text.empty()) return old_text;

    if (old_text.empty()) return std::move(new_text);

    size_t i = 1, idx = 0;
    auto l = std::min(old_text.size(), new_text.size());
    for (; i <= l; ++i) {
        auto beg = old_text.cend();
        std::advance(beg, 0 - i);

        auto end = new_text.cbegin();
        std::advance(end, i);

        if (std::equal(beg, old_text.cend(), new_text.cbegin(), end)) idx = i;
    }

    if (idx > 0) {
        new_text = new_text.substr(idx);
        ltrim(new_text);
    }

    if (new_text.empty()) return old_text;
    return old_text + " " + new_text;
}

void stt_engine::set_intermediate_text(const std::string& text) {
    if (m_intermediate_text != text) {
        m_intermediate_text = text;
        if (m_intermediate_text->empty() ||
            m_intermediate_text->size() >= m_min_text_size) {
            m_call_backs.intermediate_text_decoded(m_intermediate_text.value());
        }
    }
}

stt_engine::speech_detection_status_t stt_engine::speech_detection_status()
    const {
    switch (m_processing_state) {
        case processing_state_t::initializing:
            return speech_detection_status_t::initializing;
        case processing_state_t::decoding:
            if (m_speech_detection_status ==
                speech_detection_status_t::speech_detected)
                break;
            else
                return speech_detection_status_t::decoding;
        case processing_state_t::idle:
            break;
    }

    return m_speech_detection_status;
}

void stt_engine::set_processing_state(processing_state_t new_state) {
    if (m_processing_state != new_state) {
        auto old_speech_status = speech_detection_status();

        LOGD("processing state: " << m_processing_state << " => " << new_state);

        m_processing_state = new_state;

        auto new_speech_status = speech_detection_status();

        if (old_speech_status != new_speech_status) {
            LOGD("speech detection status: "
                 << old_speech_status << " => " << new_speech_status << " ("
                 << m_speech_detection_status << ")");
            m_call_backs.speech_detection_status_changed(new_speech_status);
        }
    }
}

void stt_engine::flush(flush_t type) {
    LOGD("flush: " << type);

    if (m_speech_mode == speech_mode_t::automatic) {
        set_speech_detection_status(speech_detection_status_t::no_speech);
    } else if (type != flush_t::restart &&
               m_speech_mode == speech_mode_t::manual) {
        set_speech_started(false);
    }

    if (m_intermediate_text && !m_intermediate_text->empty()) {
        if ((type == flush_t::regular || type == flush_t::eof ||
             m_speech_mode != speech_mode_t::single_sentence) &&
            m_intermediate_text->size() >= m_min_text_size) {
            m_call_backs.text_decoded(m_intermediate_text.value());

            if (m_speech_mode == speech_mode_t::single_sentence) {
                set_speech_started(false);
            }
        }
        set_intermediate_text("");
    }

    m_intermediate_text.reset();

    if (type == flush_t::eof) {
        m_call_backs.eof();
    }
}

void stt_engine::set_speech_mode(speech_mode_t mode) {
    if (m_speech_mode != mode) {
        LOGD("speech mode: " << m_speech_mode << " => " << mode);

        m_speech_mode = mode;
        set_speech_started(false);
    }
}

void stt_engine::set_speech_started(bool value) {
    if (m_speech_started != value) {
        LOGD("speech started: " << m_speech_started << " => " << value);

        m_speech_started = value;
        m_start_time.reset();
        if (m_speech_mode == speech_mode_t::manual ||
            m_speech_mode == speech_mode_t::single_sentence) {
            set_speech_detection_status(
                value ? speech_detection_status_t::speech_detected
                      : speech_detection_status_t::no_speech);
        }
    }
}

void stt_engine::set_speech_detection_status(speech_detection_status_t status) {
    if (m_speech_detection_status == status) return;

    auto old_speech_status = speech_detection_status();

    m_speech_detection_status = status;

    auto new_speech_status = speech_detection_status();

    LOGD("speech detection status: " << old_speech_status << " => "
                                     << new_speech_status << " ("
                                     << m_speech_detection_status << ")");

    if (old_speech_status != new_speech_status)
        m_call_backs.speech_detection_status_changed(new_speech_status);
}

void stt_engine::ltrim(std::string& s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
                return !std::isspace(ch);
            }));
}

void stt_engine::rtrim(std::string& s) {
    s.erase(std::find_if(s.rbegin(), s.rend(),
                         [](unsigned char ch) { return !std::isspace(ch); })
                .base(),
            s.end());
}

bool stt_engine::sentence_timer_timed_out() {
    if (m_start_time) {
        if (std::chrono::steady_clock::now() - *m_start_time >= m_timeout) {
            return true;
        }
    } else {
        restart_sentence_timer();
    }

    return false;
}

void stt_engine::restart_sentence_timer() {
    LOGT("staring sentence timer");
    m_start_time = std::chrono::steady_clock::now();
}
