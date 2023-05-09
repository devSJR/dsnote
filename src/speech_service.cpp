﻿/* Copyright (C) 2021-2023 Michal Kosciesza <michal@mkiol.net>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "speech_service.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDebug>
#include <QEventLoop>
#include <algorithm>
#include <functional>

#include "coqui_engine.hpp"
#include "ds_engine.hpp"
#include "file_source.h"
#include "mic_source.h"
#include "piper_engine.hpp"
#include "settings.h"
#include "vosk_engine.hpp"
#include "whisper_engine.hpp"

QDebug operator<<(QDebug d, speech_service::state_t state_value) {
    switch (state_value) {
        case speech_service::state_t::busy:
            d << "busy";
            break;
        case speech_service::state_t::idle:
            d << "idle";
            break;
        case speech_service::state_t::listening_manual:
            d << "listening-manual";
            break;
        case speech_service::state_t::listening_auto:
            d << "listening-auto";
            break;
        case speech_service::state_t::not_configured:
            d << "not-configured";
            break;
        case speech_service::state_t::transcribing_file:
            d << "transcribing-file";
            break;
        case speech_service::state_t::listening_single_sentence:
            d << "listening-single-sentence";
            break;
        case speech_service::state_t::playing_speech:
            d << "playing-speech";
            break;
        case speech_service::state_t::unknown:
            d << "unknown";
            break;
    }

    return d;
}

speech_service::speech_service(QObject *parent)
    : QObject{parent}, m_dbus_service_adaptor{this} {
    qDebug() << "starting service:" << settings::instance()->launch_mode();

    connect(this, &speech_service::models_changed, this,
            &speech_service::refresh_status);
    connect(models_manager::instance(), &models_manager::models_changed, this,
            &speech_service::handle_models_changed);
    connect(models_manager::instance(), &models_manager::busy_changed, this,
            &speech_service::refresh_status);
    connect(models_manager::instance(), &models_manager::download_progress,
            this, [this](const QString &id, double progress) {
                emit model_download_progress(id, progress);
            });
    connect(this, &speech_service::stt_text_decoded, this,
            static_cast<void (speech_service::*)(const QString &,
                                                 const QString &, int)>(
                &speech_service::handle_stt_text_decoded),
            Qt::QueuedConnection);
    connect(this, &speech_service::sentence_timeout, this,
            static_cast<void (speech_service::*)(int)>(
                &speech_service::handle_stt_sentence_timeout),
            Qt::QueuedConnection);
    connect(this, &speech_service::stt_engine_eof, this,
            static_cast<void (speech_service::*)(int)>(
                &speech_service::handle_stt_engine_eof),
            Qt::QueuedConnection);
    connect(this, &speech_service::stt_engine_error, this,
            static_cast<void (speech_service::*)(int)>(
                &speech_service::handle_stt_engine_error),
            Qt::QueuedConnection);
    connect(this, &speech_service::tts_engine_error, this,
            static_cast<void (speech_service::*)(int)>(
                &speech_service::handle_tts_engine_error),
            Qt::QueuedConnection);
    connect(this, &speech_service::tts_speech_encoded, this,
            static_cast<void (speech_service::*)(const QString &, int)>(
                &speech_service::handle_tts_speech_encoded),
            Qt::QueuedConnection);
    connect(this, &speech_service::stt_engine_shutdown, this,
            [this] { stop_stt_engine(); });
    connect(
        settings::instance(), &settings::default_stt_model_changed, this,
        [this]() {
            if (settings::instance()->launch_mode() ==
                settings::launch_mode_t::service) {
                auto model = default_stt_model();
                qDebug() << "[service => dbus] signal "
                            "DefaultSttModelPropertyChanged:"
                         << model;
                emit DefaultSttModelPropertyChanged(model);
            }

            emit default_stt_model_changed();

            if (settings::instance()->launch_mode() ==
                settings::launch_mode_t::service) {
                auto lang = default_stt_lang();
                qDebug()
                    << "[service => dbus] signal DefaultSttLangPropertyChanged:"
                    << lang;
                emit DefaultSttLangPropertyChanged(lang);
            }

            emit default_stt_lang_changed();
        },
        Qt::QueuedConnection);
    connect(
        settings::instance(), &settings::default_tts_model_changed, this,
        [this]() {
            if (settings::instance()->launch_mode() ==
                settings::launch_mode_t::service) {
                auto model = default_tts_model();
                qDebug() << "[service => dbus] signal "
                            "DefaultTtsModelPropertyChanged:"
                         << model;
                emit DefaultTtsModelPropertyChanged(model);
            }

            emit default_tts_model_changed();

            if (settings::instance()->launch_mode() ==
                settings::launch_mode_t::service) {
                auto lang = default_tts_lang();
                qDebug()
                    << "[service => dbus] signal DefaultTtsLangPropertyChanged:"
                    << lang;
                emit DefaultTtsLangPropertyChanged(lang);
            }

            emit default_tts_lang_changed();
        },
        Qt::QueuedConnection);
    connect(
        &m_player, &QMediaPlayer::stateChanged, this,
        [this](QMediaPlayer::State new_state) {
            qDebug() << "player new state:" << new_state;

            update_speech_state();

            if (new_state == QMediaPlayer::State::StoppedState &&
                m_current_task && m_current_task->engine == engine_t::tts)
                tts_stop_speech(m_current_task->id);
        },
        Qt::QueuedConnection);

    if (settings::instance()->launch_mode() ==
        settings::launch_mode_t::service) {
        connect(
            this, &speech_service::state_changed, this,
            [this]() {
                qDebug() << "[service => dbus] signal StatePropertyChanged:"
                         << dbus_state();
                emit StatePropertyChanged(dbus_state());
            },
            Qt::QueuedConnection);
        connect(
            this, &speech_service::current_task_changed, this,
            [this]() {
                qDebug()
                    << "[service => dbus] signal CurrentTaskPropertyChanged:"
                    << current_task_id();
                emit CurrentTaskPropertyChanged(current_task_id());
            },
            Qt::QueuedConnection);
        connect(this, &speech_service::error, this, [this](error_t type) {
            qDebug() << "[service => dbus] signal ErrorOccured:"
                     << static_cast<int>(type);
            emit ErrorOccured(static_cast<int>(type));
        });
        connect(this, &speech_service::stt_file_transcribe_finished, this,
                [this](int task) {
                    qDebug()
                        << "[service => dbus] signal SttFileTranscribeFinished:"
                        << task;
                    emit SttFileTranscribeFinished(task);
                });
        connect(
            this, &speech_service::stt_intermediate_text_decoded, this,
            [this](const QString &text, const QString &lang, int task) {
                qDebug()
                    << "[service => dbus] signal SttIntermediateTextDecoded:"
                    << lang << task;
                emit SttIntermediateTextDecoded(text, lang, task);
            });
        connect(this, &speech_service::stt_text_decoded, this,
                [this](const QString &text, const QString &lang, int task) {
                    qDebug()
                        << "[service => dbus] signal SttTextDecoded:" << lang
                        << task;
                    emit SttTextDecoded(text, lang, task);
                });
        connect(
            this, &speech_service::speech_changed, this,
            [this]() {
                qDebug() << "[service => dbus] signal SpeechPropertyChanged:"
                         << speech();
                emit SpeechPropertyChanged(speech());
            },
            Qt::QueuedConnection);
        connect(this, &speech_service::models_changed, this, [this]() {
            auto models_list = available_stt_models();
            qDebug() << "[service => dbus] signal SttModelsPropertyChanged:"
                     << models_list;
            emit SttModelsPropertyChanged(models_list);

            auto langs_list = available_stt_langs();
            qDebug() << "[service => dbus] signal SttLangsPropertyChanged:"
                     << langs_list;
            emit SttLangsPropertyChanged(langs_list);
        });
        connect(this, &speech_service::models_changed, this, [this]() {
            auto models_list = available_tts_models();
            qDebug() << "[service => dbus] signal TtsModelsPropertyChanged:"
                     << models_list;
            emit TtsModelsPropertyChanged(models_list);

            auto langs_list = available_tts_langs();
            qDebug() << "[service => dbus] signal TtsLangsPropertyChanged:"
                     << langs_list;
            emit TtsLangsPropertyChanged(langs_list);
        });
        connect(this, &speech_service::stt_transcribe_file_progress_changed,
                this, [this](double progress, int task) {
                    qDebug()
                        << "[service => dbus] signal SttFileTranscribeProgress:"
                        << progress << task;
                    emit SttFileTranscribeProgress(progress, task);
                });
        connect(this, &speech_service::tts_play_speech_finished, this,
                [this](int task) {
                    qDebug()
                        << "[service => dbus] signal TtsPlaySpeechFinished:"
                        << task;
                    emit TtsPlaySpeechFinished(task);
                });

        m_keepalive_timer.setSingleShot(true);
        m_keepalive_timer.setTimerType(Qt::VeryCoarseTimer);
        m_keepalive_timer.setInterval(KEEPALIVE_TIME);
        connect(&m_keepalive_timer, &QTimer::timeout, this,
                &speech_service::handle_keepalive_timeout);

        m_keepalive_current_task_timer.setSingleShot(true);
        m_keepalive_current_task_timer.setTimerType(Qt::VeryCoarseTimer);
        m_keepalive_current_task_timer.setInterval(KEEPALIVE_TASK_TIME);
        connect(&m_keepalive_current_task_timer, &QTimer::timeout, this,
                &speech_service::handle_task_timeout);

        // DBus
        auto con = QDBusConnection::sessionBus();
        if (!con.registerService(DBUS_SERVICE_NAME)) {
            qWarning() << "dbus service registration failed";
            throw std::runtime_error("dbus service registration failed");
        }
        if (!con.registerObject(DBUS_SERVICE_PATH, this)) {
            qWarning() << "dbus object registration failed";
            throw std::runtime_error("dbus object registration failed");
        }
    }

    if (settings::instance()->launch_mode() ==
        settings::launch_mode_t::service) {
        m_keepalive_timer.start();
    }

    handle_models_changed();
}

speech_service::~speech_service() { qDebug() << "speech service dtor"; }

speech_service::source_t speech_service::audio_source_type() const {
    if (!m_source) return source_t::none;
    if (m_source->type() == audio_source::source_type::mic)
        return source_t::mic;
    if (m_source->type() == audio_source::source_type::file)
        return source_t::file;
    return source_t::none;
}

std::optional<speech_service::model_config_t>
speech_service::choose_model_config(engine_t engine_type,
                                    QString model_or_lang_id) {
    if (model_or_lang_id.isEmpty()) {
        switch (engine_type) {
            case engine_t::stt:
                model_or_lang_id = settings::instance()->default_stt_model();
                break;
            case engine_t::tts:
                model_or_lang_id = settings::instance()->default_tts_model();
                break;
        }
    }

    m_available_stt_models_map.clear();
    m_available_tts_models_map.clear();

    auto models = models_manager::instance()->available_models();
    if (models.empty()) return std::nullopt;

    std::optional<model_config_t> active_config;
    std::optional<model_config_t> first_config;

    // search by model id + filling m_available_*_models map
    for (const auto &model : models) {
        auto role = models_manager::role_of_engine(model.engine);

        switch (role) {
            case models_manager::model_role::stt:
                m_available_stt_models_map.emplace(
                    model.id, model_data_t{model.id, model.lang_id,
                                           model.engine, model.name});
                break;
            case models_manager::model_role::ttt:
                m_available_ttt_models_map.emplace(
                    model.id, model_data_t{model.id, model.lang_id,
                                           model.engine, model.name});
                break;
            case models_manager::model_role::tts:
                m_available_tts_models_map.emplace(
                    model.id, model_data_t{model.id, model.lang_id,
                                           model.engine, model.name});
                break;
        }

        if (engine_type == engine_t::stt &&
            role != models_manager::model_role::stt)
            continue;
        if (engine_type == engine_t::tts &&
            role != models_manager::model_role::tts)
            continue;

        auto ok = model_or_lang_id.compare(model.id, Qt::CaseInsensitive) == 0;
        if (!active_config && (!first_config || ok)) {
            model_config_t config;

            config.lang_id = model.lang_id;
            config.model_id = model.id;
            config.engine = model.engine;
            config.model_file = model.model_file;
            config.scorer_file = model.scorer_file;
            config.speaker = model.speaker;
            // TO-DO: vocoder for tts

            if (ok)
                active_config.emplace(std::move(config));
            else if (!first_config)
                first_config.emplace(std::move(config));
        }
    }

    // search by lang id
    if (!active_config) {
        int best_score = -1;
        const decltype(models)::value_type *best_model = nullptr;

        for (const auto &model : models) {
            auto role = models_manager::role_of_engine(model.engine);

            if (engine_type == engine_t::stt &&
                role != models_manager::model_role::stt)
                continue;
            if (engine_type == engine_t::tts &&
                role != models_manager::model_role::tts)
                continue;

            if (model_or_lang_id.compare(model.lang_id, Qt::CaseInsensitive) ==
                0) {
                if (model.default_for_lang) {
                    best_model = &model;
                    qDebug() << "best model is default model for lang:"
                             << model.lang_id << model.id;
                    break;
                }

                if (model.score > best_score) {
                    best_model = &model;
                    best_score = model.score;
                }
            }
        }

        if (best_model) {
            model_config_t config;

            config.lang_id = best_model->lang_id;
            config.model_id = best_model->id;
            config.engine = best_model->engine;
            config.model_file = best_model->model_file;
            config.scorer_file = best_model->scorer_file;
            config.speaker = best_model->speaker;

            active_config.emplace(std::move(config));
        }
    }

    // fallback to first model
    if (!active_config && first_config) {
        active_config.emplace(std::move(*first_config));
        qWarning() << "cannot find requested model, choosing:"
                   << active_config->model_id;
    }

    // search for ttt model for stt lang
    // only when restore punctuation is enabled
    if (engine_type == engine_t::stt && active_config &&
        settings::instance()->restore_punctuation()) {
        auto it = std::find_if(
            models.cbegin(), models.cend(), [&](const auto &model) {
                if (models_manager::role_of_engine(model.engine) !=
                    models_manager::model_role::ttt)
                    return false;
                return model.lang_id == active_config->lang_id;
            });

        if (it != models.cend()) {
            qDebug() << "found ttt model for stt:" << it->id;

            active_config->ttt_model_id = it->id;
            active_config->ttt_model_file = it->model_file;
            active_config->ttt_engine = it->engine;
        }
    }

    return active_config;
}

void speech_service::handle_models_changed() {
    choose_model_config(engine_t::tts);  // any engine is ok

    if (m_current_task &&
        (m_available_stt_models_map.find(m_current_task->model_id) ==
             m_available_stt_models_map.end() ||
         m_available_tts_models_map.find(m_current_task->model_id) ==
             m_available_tts_models_map.end())) {
        stop_stt();
    }

    emit models_changed();
}

QString speech_service::lang_from_model_id(const QString &model_id) {
    auto l = model_id.split('_');

    if (l.empty()) {
        qDebug() << "invalid model id:" << model_id;
        return {};
    }

    return l.first();
}

QString speech_service::restart_stt_engine(speech_mode_t speech_mode,
                                           const QString &model_id,
                                           bool translate) {
    if (auto model_files = choose_model_config(engine_t::stt, model_id)) {
        stt_engine::config_t config;

        config.model_files = {
            /*model_file=*/model_files->model_file.toStdString(),
            /*scorer_file=*/
            model_files->scorer_file.toStdString(),
            /*ttt_model_file=*/model_files->ttt_model_file.toStdString()};

        config.lang = model_files->lang_id.toStdString();
        config.speech_mode =
            static_cast<stt_engine::speech_mode_t>(speech_mode);
        config.translate = translate;

        bool new_engine_required = [&] {
            if (!m_stt_engine) return true;
            if (translate != m_stt_engine->translate()) return true;

            const auto &type = typeid(*m_stt_engine);
            if (model_files->engine == models_manager::model_engine::stt_ds &&
                type != typeid(ds_engine))
                return true;
            if (model_files->engine == models_manager::model_engine::stt_vosk &&
                type != typeid(vosk_engine))
                return true;
            if (model_files->engine ==
                    models_manager::model_engine::stt_whisper &&
                type != typeid(whisper_engine))
                return true;

            if (m_stt_engine->model_files() != config.model_files) return true;
            if (m_stt_engine->lang() != config.lang) return true;

            return false;
        }();

        if (new_engine_required) {
            qDebug() << "new stt engine required";

            if (m_stt_engine) {
                m_stt_engine.reset();
                qDebug() << "stt engine destroyed successfully";
            }

            stt_engine::callbacks_t call_backs{
                /*text_decoded=*/[this](const std::string &text) {
                    handle_stt_text_decoded(text);
                },
                /*intermediate_text_decoded=*/
                [this](const std::string &text) {
                    handle_stt_intermediate_text_decoded(text);
                },
                /*speech_detection_status_changed=*/
                [this](stt_engine::speech_detection_status_t status) {
                    handle_stt_speech_detection_status_changed(status);
                },
                /*sentence_timeout=*/
                [this]() { handle_stt_sentence_timeout(); },
                /*eof=*/
                [this]() { handle_stt_engine_eof(); },
                /*stopped=*/
                [this]() { handle_stt_engine_error(); }};

            switch (model_files->engine) {
                case models_manager::model_engine::stt_ds:
                    m_stt_engine = std::make_unique<ds_engine>(
                        std::move(config), std::move(call_backs));
                    break;
                case models_manager::model_engine::stt_vosk:
                    m_stt_engine = std::make_unique<vosk_engine>(
                        std::move(config), std::move(call_backs));
                    break;
                case models_manager::model_engine::stt_whisper:
                    m_stt_engine = std::make_unique<whisper_engine>(
                        std::move(config), std::move(call_backs));
                    break;
                case models_manager::model_engine::ttt_hftc:
                case models_manager::model_engine::tts_coqui:
                case models_manager::model_engine::tts_piper:
                    throw std::runtime_error{
                        "invalid model engine, expected stt"};
            }

            m_stt_engine->start();
        } else {
            qDebug() << "new stt engine not required, only restart";
            m_stt_engine->stop();
            m_stt_engine->start();
            m_stt_engine->set_speech_mode(
                static_cast<stt_engine::speech_mode_t>(speech_mode));
        }

        return model_files->model_id;
    }

    qWarning() << "failed to restart stt engine, no valid model";
    return {};
}

QString speech_service::restart_tts_engine(const QString &model_id) {
    if (auto model_config = choose_model_config(engine_t::tts, model_id)) {
        tts_engine::config_t config;

        config.model_files = {
            /*model_path=*/model_config->model_file.toStdString(),
            /*vocoder_path=*/model_config->vocoder_file.toStdString()};
        config.lang = model_config->lang_id.toStdString();
        config.cache_dir = settings::instance()->cache_dir().toStdString();
        config.speaker = model_config->speaker.toStdString();

        bool new_engine_required = [&] {
            if (!m_tts_engine) return true;

            const auto &type = typeid(*m_tts_engine);
            if (model_config->engine ==
                    models_manager::model_engine::tts_coqui &&
                type != typeid(coqui_engine))
                return true;
            if (model_config->engine ==
                    models_manager::model_engine::tts_piper &&
                type != typeid(piper_engine))
                return true;

            if (m_tts_engine->model_files() != config.model_files) return true;
            if (m_tts_engine->lang() != config.lang) return true;
            if (m_tts_engine->speaker() != config.speaker) return true;

            return false;
        }();

        if (new_engine_required) {
            qDebug() << "new tts engine required";

            if (m_tts_engine) {
                m_tts_engine.reset();
                qDebug() << "tts engine destroyed successfully";
            }

            tts_engine::callbacks_t call_backs{
                /*speech_encoded=*/[this](const std::string &wav_file_path) {
                    handle_tts_speech_encoded(wav_file_path);
                },
                /*state_changed=*/
                [this](tts_engine::state_t state) {
                    handle_tts_engine_state_changed(state);
                },
                /*error=*/
                [this]() { handle_tts_engine_error(); }};

            switch (model_config->engine) {
                case models_manager::model_engine::tts_coqui:
                    m_tts_engine = std::make_unique<coqui_engine>(
                        std::move(config), std::move(call_backs));
                    break;
                case models_manager::model_engine::tts_piper:
                    m_tts_engine = std::make_unique<piper_engine>(
                        std::move(config), std::move(call_backs));
                    break;
                case models_manager::model_engine::ttt_hftc:
                case models_manager::model_engine::stt_ds:
                case models_manager::model_engine::stt_vosk:
                case models_manager::model_engine::stt_whisper:
                    throw std::runtime_error{
                        "invalid model engine, expected tts"};
            }
        } else {
            qDebug() << "new tts engine not required";
        }

        return model_config->model_id;
    }

    qWarning() << "failed to restart tts engine, no valid model";
    return {};
}

void speech_service::handle_stt_intermediate_text_decoded(
    const std::string &text) {
    if (m_current_task) {
        m_last_intermediate_text_task = m_current_task->id;
        emit stt_intermediate_text_decoded(QString::fromStdString(text),
                                           m_current_task->model_id,
                                           m_current_task->id);
    } else {
        qWarning() << "current task does not exist";
    }
}

void speech_service::handle_stt_text_decoded(const QString &, const QString &,
                                             int task_id) {
    if (m_current_task && m_current_task->id == task_id &&
        m_current_task->speech_mode == speech_mode_t::single_sentence) {
        stt_stop_listen(m_current_task->id);
    }
}

void speech_service::handle_stt_sentence_timeout(int task_id) {
    stt_stop_listen(task_id);
}

void speech_service::handle_stt_sentence_timeout() {
    if (m_current_task &&
        m_current_task->speech_mode == speech_mode_t::single_sentence) {
        emit sentence_timeout(m_current_task->id);
    }
}

void speech_service::handle_stt_engine_eof(int task_id) {
    qDebug() << "engine eof";
    emit stt_file_transcribe_finished(task_id);
    cancel(task_id);
}

void speech_service::handle_stt_engine_eof() {
    if (m_current_task) emit stt_engine_eof(m_current_task->id);
}

void speech_service::handle_stt_engine_error(int task_id) {
    qDebug() << "stt engine error";

    if (current_task_id() == task_id) {
        cancel(task_id);
        if (m_stt_engine) {
            m_stt_engine.reset();
            qDebug() << "stt engine destroyed successfully";
        }
    }
}

void speech_service::handle_stt_engine_error() {
    if (m_current_task) emit stt_engine_error(m_current_task->id);
}

void speech_service::handle_tts_engine_error(int task_id) {
    qDebug() << "tts engine error";

    if (current_task_id() == task_id) {
        cancel(task_id);
        if (m_stt_engine) {
            m_stt_engine.reset();
            qDebug() << "tts engine destroyed successfully";
        }
    }
}

void speech_service::handle_tts_engine_state_changed(
    [[maybe_unused]] tts_engine::state_t state) {
    qDebug() << "tts engine state changed";
    update_speech_state();
}

void speech_service::handle_tts_speech_encoded(
    const std::string &wav_file_path) {
    if (m_current_task)
        emit tts_speech_encoded(QString::fromStdString(wav_file_path),
                                m_current_task->id);
}

void speech_service::handle_tts_speech_encoded(const QString &wav_file_path,
                                               int task_id) {
    m_player.setMedia(QMediaContent{QUrl::fromLocalFile(wav_file_path)});
    m_player.play();

    emit tts_play_speech_finished(task_id);
}

void speech_service::handle_tts_engine_error() {
    if (m_current_task) emit tts_engine_error(m_current_task->id);
}

void speech_service::handle_stt_text_decoded(const std::string &text) {
    if (m_current_task) {
        if (m_previous_task &&
            m_last_intermediate_text_task == m_previous_task->id) {
            emit stt_text_decoded(QString::fromStdString(text),
                                  m_previous_task->model_id,
                                  m_previous_task->id);
        } else {
            emit stt_text_decoded(QString::fromStdString(text),
                                  m_current_task->model_id, m_current_task->id);
        }
    } else {
        qWarning() << "current task does not exist";
    }

    m_previous_task.reset();
}

void speech_service::handle_stt_speech_detection_status_changed(
    [[maybe_unused]] stt_engine::speech_detection_status_t status) {
    update_speech_state();
}

QVariantMap speech_service::available_models(
    const std::map<QString, model_data_t> &available_models_map) const {
    QVariantMap map;

    std::for_each(available_models_map.cbegin(), available_models_map.cend(),
                  [&map](const auto &p) {
                      map.insert(
                          p.first,
                          QStringList{p.second.model_id,
                                      QStringLiteral("%1 / %2").arg(
                                          p.second.name, p.second.lang_id)});
                  });

    return map;
}

QVariantMap speech_service::available_langs(
    const std::map<QString, model_data_t> &available_models_map) const {
    QVariantMap map;

    std::for_each(
        available_models_map.cbegin(), available_models_map.cend(),
        [&map](const auto &p) {
            if (!map.contains(p.second.lang_id)) {
                map.insert(p.second.lang_id,
                           QStringList{p.second.model_id,
                                       QStringLiteral("%1 / %2").arg(
                                           p.second.name, p.second.lang_id)});
            }
        });

    return map;
}

QVariantMap speech_service::available_stt_models() const {
    return available_models(m_available_stt_models_map);
}

QVariantMap speech_service::available_tts_models() const {
    return available_models(m_available_tts_models_map);
}

QVariantMap speech_service::available_ttt_models() const {
    return available_models(m_available_ttt_models_map);
}

QVariantMap speech_service::available_stt_langs() const {
    return available_langs(m_available_stt_models_map);
}

QVariantMap speech_service::available_tts_langs() const {
    return available_langs(m_available_tts_models_map);
}

QVariantMap speech_service::available_ttt_langs() const {
    return available_langs(m_available_ttt_models_map);
}

void speech_service::download_model(const QString &id) {
    models_manager::instance()->download_model(id);
}

void speech_service::delete_model(const QString &id) {
    if (m_current_task && m_current_task->model_id == id) stop_stt();
    models_manager::instance()->delete_model(id);
}

void speech_service::handle_audio_available() {
    if (m_source && m_stt_engine && m_stt_engine->started()) {
        if (m_stt_engine->speech_detection_status() ==
            stt_engine::speech_detection_status_t::initializing) {
            if (m_source->type() == audio_source::source_type::mic)
                m_source->clear();
            return;
        }

        auto [buf, max_size] = m_stt_engine->borrow_buf();

        if (buf) {
            auto audio_data = m_source->read_audio(buf, max_size);

            if (audio_data.eof) qDebug() << "audio eof";

            m_stt_engine->return_buf(buf, audio_data.size, audio_data.sof,
                                     audio_data.eof);
            set_progress(m_source->progress());
        }
    }
}

void speech_service::set_progress(double p) {
    if (audio_source_type() == source_t::file && m_current_task) {
        const auto delta = p - m_progress;
        if (delta > 0.01 || p < 0.0 || p >= 1) {
            m_progress = p;
            emit stt_transcribe_file_progress_changed(m_progress,
                                                      m_current_task->id);
        }
    }
}

double speech_service::stt_transcribe_file_progress(int task) const {
    if (audio_source_type() == source_t::file) {
        if (m_current_task && m_current_task->id == task) {
            return m_progress;
        }
        qWarning() << "invalid task id";
    }

    return -1.0;
}

int speech_service::next_task_id() {
    m_last_task_id = (m_last_task_id + 1) % std::numeric_limits<int>::max();
    return m_last_task_id;
}

int speech_service::stt_transcribe_file(const QString &file,
                                        const QString &lang, bool translate) {
    if (state() == state_t::unknown || state() == state_t::not_configured ||
        state() == state_t::busy) {
        qWarning() << "cannot transcribe_file, invalid state";
        return INVALID_TASK;
    }

    if (m_current_task &&
        m_current_task->speech_mode != speech_mode_t::single_sentence &&
        audio_source_type() == source_t::mic) {
        m_pending_task = m_current_task;
    }

    m_current_task = {
        next_task_id(), engine_t::stt,
        restart_stt_engine(speech_mode_t::automatic, lang, translate),
        speech_mode_t::automatic, translate};

    if (QFileInfo::exists(file)) {
        restart_audio_source(file);
    } else {
        restart_audio_source(QUrl{file}.toLocalFile());
    }

    start_keepalive_current_task();

    emit current_task_changed();

    refresh_status();

    return m_current_task->id;
}

int speech_service::stt_start_listen(speech_mode_t mode, const QString &lang,
                                     bool translate) {
    if (state() == state_t::unknown || state() == state_t::not_configured ||
        state() == state_t::busy) {
        qWarning() << "cannot stt start listen, invalid state";
        return INVALID_TASK;
    }

    if (audio_source_type() == source_t::file) {
        m_pending_task = {next_task_id(), engine_t::stt, lang, mode};
        return m_pending_task->id;
    }

    m_current_task = {next_task_id(), engine_t::stt,
                      restart_stt_engine(mode, lang, translate), mode,
                      translate};
    restart_audio_source();
    if (m_stt_engine) m_stt_engine->set_speech_started(true);

    start_keepalive_current_task();

    emit current_task_changed();

    refresh_status();

    return m_current_task->id;
}

int speech_service::tts_play_speech(const QString &text, const QString &lang) {
    if (state() == state_t::unknown || state() == state_t::not_configured ||
        state() == state_t::busy) {
        qWarning() << "cannot tts play speech, invalid state";
        return INVALID_TASK;
    }

    m_current_task = {next_task_id(), engine_t::tts, restart_tts_engine(lang),
                      speech_mode_t::single_sentence, false};

    if (m_tts_engine) m_tts_engine->encode_speech(text.toStdString());

    start_keepalive_current_task();

    emit current_task_changed();

    refresh_status();

    return m_current_task->id;
}

int speech_service::cancel(int task) {
    if (state() == state_t::unknown || state() == state_t::not_configured ||
        state() == state_t::busy) {
        qWarning() << "cannot cancel, invalid state";
        return FAILURE;
    }

    qDebug() << "cancel";

    if (audio_source_type() == source_t::file) {
        if (m_current_task && m_current_task->id == task) {
            if (m_pending_task) {
                m_previous_task = m_current_task;
                restart_stt_engine(m_pending_task->speech_mode,
                                   m_pending_task->model_id,
                                   m_pending_task->translate);
                restart_audio_source();
                m_current_task = m_pending_task;
                start_keepalive_current_task();
                m_pending_task.reset();
                emit current_task_changed();
            } else {
                stop_stt_engine();
                stop_keepalive_current_task();
            }
        } else {
            qWarning() << "invalid task id";
            return FAILURE;
        }
    } else if (audio_source_type() == source_t::mic) {
        if (m_current_task && m_current_task->id == task) {
            if (m_current_task->engine != engine_t::stt) {
                qWarning() << "valid task id but invalid engine";
                return FAILURE;
            }

            if (m_current_task->speech_mode == speech_mode_t::automatic) {
                restart_stt_engine(m_current_task->speech_mode,
                                   m_current_task->model_id,
                                   m_current_task->translate);
                restart_audio_source();
            } else {
                stop_keepalive_current_task();
                stop_stt_engine();
            }
        } else {
            qWarning() << "invalid task id";
            return FAILURE;
        }
    } else {
        if (m_current_task && m_current_task->id == task &&
            m_current_task->engine == engine_t::tts) {
            stop_keepalive_current_task();
            stop_tts_engine();
            m_player.stop();
        } else {
            qWarning() << "invalid task id";
            return FAILURE;
        }
    }

    refresh_status();
    return SUCCESS;
}

int speech_service::stt_stop_listen(int task) {
    if (state() == state_t::unknown || state() == state_t::not_configured ||
        state() == state_t::busy) {
        qWarning() << "cannot stop_listen, invalid state";
        return FAILURE;
    }

    if (audio_source_type() == source_t::file) {
        if (m_pending_task && m_pending_task->id == task)
            m_pending_task.reset();
        else
            qWarning() << "invalid task id";
    } else if (audio_source_type() == source_t::mic) {
        if (m_current_task && m_current_task->id == task) {
            if (m_current_task->engine != engine_t::stt) {
                qWarning() << "valid task id but invalid engine";
                return FAILURE;
            }

            stop_keepalive_current_task();
            if (m_current_task->speech_mode == speech_mode_t::single_sentence ||
                m_current_task->speech_mode == speech_mode_t::automatic) {
                stop_stt_engine();
            } else if (m_stt_engine && m_stt_engine->started()) {
                stop_stt_engine_gracefully();
            } else {
                stop_stt_engine();
            }
        } else {
            qWarning() << "invalid task id";
            return FAILURE;
        }
    } else {
        if (m_current_task && m_current_task->id == task &&
            m_current_task->engine != engine_t::stt) {
            qWarning() << "valid task id but invalid engine";
            return FAILURE;
        }
    }

    return SUCCESS;
}

int speech_service::tts_stop_speech(int task) {
    if (state() == state_t::unknown || state() == state_t::not_configured ||
        state() == state_t::busy) {
        qWarning() << "cannot stop_listen, invalid state";
        return FAILURE;
    }

    if (!m_current_task || m_current_task->id != task) {
        qWarning() << "invalid task id";
        return FAILURE;
    }

    if (m_current_task->engine != engine_t::tts) {
        qWarning() << "valid task id but invalid engine";
        return FAILURE;
    }

    stop_tts_engine();
    m_player.stop();

    return SUCCESS;
}

void speech_service::stop_stt_engine_gracefully() {
    qDebug() << "stop stt engine gracefully";

    if (m_source) {
        if (m_stt_engine) m_stt_engine->set_speech_started(false);
        m_source->stop();
    } else {
        stop_stt_engine();
    }
}

void speech_service::stop_tts_engine() {
    qDebug() << "stop tts engine";

    m_pending_task.reset();

    if (m_current_task) {
        m_current_task.reset();
        stop_keepalive_current_task();
        emit current_task_changed();
    }

    refresh_status();
}

void speech_service::stop_stt_engine() {
    qDebug() << "stop stt engine";

    if (m_stt_engine) m_stt_engine->stop();

    restart_audio_source();

    m_pending_task.reset();

    if (m_current_task) {
        m_current_task.reset();
        stop_keepalive_current_task();
        emit current_task_changed();
    }

    refresh_status();
}

void speech_service::stop_stt() {
    qDebug() << "stop stt";
    stop_stt_engine();
}

void speech_service::handle_audio_error() {
    if (audio_source_type() == source_t::file && m_current_task) {
        qWarning() << "file audio source error";
        emit error(error_t::file_source);
        cancel(m_current_task->id);
    } else {
        qWarning() << "audio source error";
        emit error(error_t::mic_source);
        stop_stt();
    }
}

void speech_service::handle_audio_ended() {
    if (audio_source_type() == source_t::file && m_current_task) {
        qDebug() << "file audio source ended successfuly";
    } else {
        qDebug() << "audio source ended successfuly";
    }
}

void speech_service::restart_audio_source(const QString &source_file) {
    if (m_stt_engine && m_stt_engine->started()) {
        qDebug() << "creating audio source";

        if (m_source) m_source->disconnect();

        if (source_file.isEmpty())
            m_source = std::make_unique<mic_source>();
        else
            m_source = std::make_unique<file_source>(source_file);

        set_progress(m_source->progress());
        connect(m_source.get(), &audio_source::audio_available, this,
                &speech_service::handle_audio_available, Qt::QueuedConnection);
        connect(m_source.get(), &audio_source::error, this,
                &speech_service::handle_audio_error, Qt::QueuedConnection);
        connect(m_source.get(), &audio_source::ended, this,
                &speech_service::handle_audio_ended, Qt::QueuedConnection);
    } else if (m_source) {
        m_source.reset();
        set_progress(-1.0);
    }
}

void speech_service::handle_keepalive_timeout() {
    qWarning() << "keepalive timeout => shutting down";
    QCoreApplication::quit();
}

void speech_service::handle_task_timeout() {
    if (m_current_task) {
        qWarning() << "task timeout:" << m_current_task->id;
        if (m_current_task->speech_mode == speech_mode_t::single_sentence)
            stop_keepalive_current_task();
        if (audio_source_type() == source_t::file ||
            audio_source_type() == source_t::mic) {
            cancel(m_current_task->id);
        } else {
            m_current_task.reset();
            emit current_task_changed();
        }
    }
}

void speech_service::update_speech_state() {
    // 0 = No Speech
    // 1 = Speech detected
    // 2 = Speech decoding/encoding in progress
    // 3 = Speech model initialization
    // 4 = Playing Speech

    auto new_speech_state = [&] {
        if (m_stt_engine && m_stt_engine->started()) {
            switch (m_stt_engine->speech_detection_status()) {
                case stt_engine::speech_detection_status_t::speech_detected:
                    return 1;
                case stt_engine::speech_detection_status_t::decoding:
                    return 2;
                case stt_engine::speech_detection_status_t::initializing:
                    return 3;
                case stt_engine::speech_detection_status_t::no_speech:
                    break;
            }
        } else if (m_tts_engine &&
                   m_tts_engine->state() != tts_engine::state_t::idle) {
            switch (m_tts_engine->state()) {
                case tts_engine::state_t::encoding:
                    return 2;
                case tts_engine::state_t::initializing:
                    return 3;
                case tts_engine::state_t::error:
                case tts_engine::state_t::idle:
                    break;
            }
        } else if (m_player.state() == QMediaPlayer::State::PlayingState) {
            return 4;
        }

        return 0;
    }();

    if (m_speech_state != new_speech_state) {
        qDebug() << "speech state changed:" << m_speech_state << "=>"
                 << new_speech_state;
        m_speech_state = new_speech_state;
        emit speech_changed();
    }
}

void speech_service::set_state(state_t new_state) {
    if (new_state != m_state) {
        qDebug() << "state changed:" << m_state << "=>" << new_state;
        m_state = new_state;
        emit state_changed();
    }
}

void speech_service::refresh_status() {
    state_t new_state;

    if (models_manager::instance()->busy()) {
        new_state = state_t::busy;
    } else if (!models_manager::instance()->has_model_of_role(
                   models_manager::model_role::stt) &&
               !models_manager::instance()->has_model_of_role(
                   models_manager::model_role::tts)) {
        new_state = state_t::not_configured;
    } else if (audio_source_type() == source_t::file) {
        new_state = state_t::transcribing_file;
    } else if (audio_source_type() == source_t::mic) {
        if (!m_current_task) {
            qWarning() << "no current task but source is mic";
            return;
        }

        if (m_current_task->engine == engine_t::tts) {
            new_state = state_t::playing_speech;
        } else if (m_current_task->speech_mode == speech_mode_t::manual) {
            new_state = m_stt_engine && m_stt_engine->started() &&
                                m_stt_engine->speech_status()
                            ? state_t::listening_manual
                            : state_t::idle;
        } else if (m_current_task->speech_mode == speech_mode_t::automatic) {
            new_state = state_t::listening_auto;
        } else if (m_current_task->speech_mode ==
                   speech_mode_t::single_sentence) {
            new_state = state_t::listening_single_sentence;
        } else {
            qWarning() << "unknown speech mode";
            new_state = state_t::unknown;
        }
    } else if (m_current_task && m_current_task->engine == engine_t::tts) {
        new_state = state_t::playing_speech;
    } else {
        new_state = state_t::idle;
    }

    set_state(new_state);
}

QString speech_service::default_stt_model() const {
    return test_default_stt_model(settings::instance()->default_stt_model());
}

QString speech_service::default_stt_lang() const {
    return m_available_stt_models_map
        .at(test_default_stt_model(settings::instance()->default_stt_model()))
        .lang_id;
}

QString speech_service::default_tts_model() const {
    return test_default_tts_model(settings::instance()->default_tts_model());
}

QString speech_service::default_tts_lang() const {
    return m_available_tts_models_map
        .at(test_default_tts_model(settings::instance()->default_tts_model()))
        .lang_id;
}

QString speech_service::default_ttt_model() const {
    return test_default_ttt_model({});
}

QString speech_service::default_ttt_lang() const {
    return m_available_ttt_models_map.at(test_default_ttt_model({})).lang_id;
}

QString speech_service::test_default_model(
    const QString &lang,
    const std::map<QString, model_data_t> &available_models_map) {
    if (available_models_map.empty()) return {};

    auto it = available_models_map.find(lang);

    if (it == available_models_map.cend()) {
        it = std::find_if(
            available_models_map.cbegin(), available_models_map.cend(),
            [&lang](const auto &p) { return p.second.lang_id == lang; });
        if (it != available_models_map.cend()) return it->first;
    } else {
        return it->first;
    }

    return available_models_map.cbegin()->first;
}

QString speech_service::test_default_stt_model(const QString &lang) const {
    return test_default_model(lang, m_available_stt_models_map);
}

QString speech_service::test_default_tts_model(const QString &lang) const {
    return test_default_model(lang, m_available_tts_models_map);
}

QString speech_service::test_default_ttt_model(const QString &lang) const {
    return test_default_model(lang, m_available_ttt_models_map);
}

void speech_service::set_default_stt_model(const QString &model_id) const {
    if (test_default_stt_model(model_id) == model_id) {
        settings::instance()->set_default_stt_model(model_id);
    } else {
        qWarning() << "invalid default stt model";
    }
}

void speech_service::set_default_stt_lang(const QString &lang_id) const {
    settings::instance()->set_default_stt_model(
        test_default_stt_model(lang_id));
}

void speech_service::set_default_tts_model(const QString &model_id) const {
    if (test_default_tts_model(model_id) == model_id) {
        settings::instance()->set_default_tts_model(model_id);
    } else {
        qWarning() << "invalid default tts model";
    }
}

void speech_service::set_default_tts_lang(const QString &lang_id) const {
    settings::instance()->set_default_tts_model(
        test_default_tts_model(lang_id));
}

QVariantMap speech_service::translations() const {
    QVariantMap map;

    map.insert(QStringLiteral("lang_not_conf"),
               tr("Language model is not set"));
    map.insert(QStringLiteral("say_smth"), tr("Say something..."));
    map.insert(QStringLiteral("press_say_smth"),
               tr("Press and say something..."));
    map.insert(QStringLiteral("click_say_smth"),
               tr("Click and say something..."));
    map.insert(QStringLiteral("busy_stt"), tr("Busy..."));
    map.insert(QStringLiteral("decoding"), tr("Decoding, please wait..."));
    map.insert(QStringLiteral("initializing"),
               tr("Getting ready, please wait..."));

    return map;
}

speech_service::state_t speech_service::state() const { return m_state; }

int speech_service::current_task_id() const {
    return m_current_task ? m_current_task->id : INVALID_TASK;
}

int speech_service::dbus_state() const { return static_cast<int>(state()); }

void speech_service::start_keepalive_current_task() {
    if (settings::instance()->launch_mode() !=
        settings::launch_mode_t::service)
        return;

    m_keepalive_current_task_timer.start();
}

void speech_service::stop_keepalive_current_task() {
    if (settings::instance()->launch_mode() !=
        settings::launch_mode_t::service)
        return;

    m_keepalive_current_task_timer.stop();
}

// DBus

int speech_service::SttStartListen(int mode, const QString &lang,
                                   bool translate) {
    qDebug() << "[dbus => service] called StartListen:" << lang << mode;
    m_keepalive_timer.start();

    return stt_start_listen(static_cast<speech_mode_t>(mode), lang, translate);
}

int speech_service::SttStopListen(int task) {
    qDebug() << "[dbus => service] called StopListen:" << task;
    m_keepalive_timer.start();

    return stt_stop_listen(task);
}

int speech_service::Cancel(int task) {
    qDebug() << "[dbus => service] called Cancel:" << task;
    m_keepalive_timer.start();

    return cancel(task);
}

int speech_service::SttTranscribeFile(const QString &file, const QString &lang,
                                      bool translate) {
    qDebug() << "[dbus => service] called TranscribeFile:" << file << lang;
    start_keepalive_current_task();

    return stt_transcribe_file(file, lang, translate);
}

double speech_service::SttGetFileTranscribeProgress(int task) {
    qDebug() << "[dbus => service] called GetFileTranscribeProgress:" << task;
    start_keepalive_current_task();

    return stt_transcribe_file_progress(task);
}

int speech_service::KeepAliveService() {
    qDebug() << "[dbus => service] called KeepAliveService";
    m_keepalive_timer.start();

    return m_keepalive_timer.remainingTime();
}

int speech_service::KeepAliveTask(int task) {
    qDebug() << "[dbus => service] called KeepAliveTask:" << task;
    m_keepalive_timer.start();

    if (m_current_task && m_current_task->id == task) {
        m_keepalive_current_task_timer.start();
        return m_keepalive_current_task_timer.remainingTime();
    }
    if (m_pending_task && m_pending_task->id == task) {
        qDebug() << "pending:" << task;
        return KEEPALIVE_TASK_TIME;
    }

    qWarning() << "invalid task:" << task;

    return 0;
}

int speech_service::TtsPlaySpeech(const QString &text, const QString &lang) {
    qDebug() << "[dbus => service] called TtsPlaySpeech:" << lang;
    start_keepalive_current_task();

    return tts_play_speech(text, lang);
}

int speech_service::TtsStopSpeech(int task) {
    qDebug() << "[dbus => service] called TtsStopSpeech";
    start_keepalive_current_task();

    return tts_stop_speech(task);
}

int speech_service::Reload() {
    qDebug() << "[dbus => service] called Reload";
    m_keepalive_timer.start();

    models_manager::instance()->reload();
    return SUCCESS;
}