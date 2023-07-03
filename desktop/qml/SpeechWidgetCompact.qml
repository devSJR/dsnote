/* Copyright (C) 2021-2023 Michal Kosciesza <michal@mkiol.net>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

import QtQuick 2.0
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.3
import QtQuick.Window 2.2

import org.mkiol.dsnote.Dsnote 1.0
import org.mkiol.dsnote.Settings 1.0

ColumnLayout {
    Layout.fillWidth: true
    Layout.rightMargin: appWin.padding
    Layout.leftMargin: appWin.padding
    Layout.bottomMargin: appWin.padding
    spacing: appWin.padding

    RowLayout {
        Layout.fillWidth: true
        spacing: appWin.padding
        BusyIndicator {
            Layout.preferredHeight: listenButton.height
            Layout.preferredWidth: listenButton.height
            Layout.alignment: frame.height > listenButton.height ? Qt.AlignBottom : Qt.AlignVCenter
            running: app.state === DsnoteApp.StateTranscribingFile ||
                     app.state === DsnoteApp.StateWritingSpeechToFile
            visible: running
        }

        SpeechIndicator {
            id: indicator
            Layout.preferredHeight: listenButton.height * 0.7
            Layout.preferredWidth: listenButton.height
            visible: app.state !== DsnoteApp.StateTranscribingFile &&
                     app.state !== DsnoteApp.StateWritingSpeechToFile
            status: {
                switch (app.speech) {
                case DsnoteApp.SpeechStateNoSpeech: return 0;
                case DsnoteApp.SpeechStateSpeechDetected: return 1;
                case DsnoteApp.SpeechStateSpeechDecodingEncoding: return 2;
                case DsnoteApp.SpeechStateSpeechInitializing: return 3;
                case DsnoteApp.SpeechStateSpeechPlaying: return 4;
                }
                return 0;
            }
            Layout.alignment: frame.height > listenButton.height ? Qt.AlignBottom : Qt.AlignVCenter
            color: palette.text
        }

        Frame {
            id: frame
            topPadding: 2
            bottomPadding: 2
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(listenButton.height,
                                             speechText.implicitHeight + topPadding + bottomPadding)
            background: Rectangle {
                color: palette.button
                border.color: palette.buttonText
                opacity: 0.3
                radius: 3
            }

            Label {
                id: speechText
                anchors.fill: parent
                wrapMode: TextEdit.WordWrap
                verticalAlignment: Text.AlignVCenter

                property string placeholderText: {
                    if (app.speech === DsnoteApp.SpeechStateSpeechInitializing)
                        return qsTr("Getting ready, please wait...")
                    if (app.state === DsnoteApp.StateWritingSpeechToFile)
                        return qsTr("Writing speech to file...") +
                                (app.speech_to_file_progress > 0.0 ? " " +
                                                                     Math.round(app.speech_to_file_progress * 100) + "%" : "")
                    if (app.speech === DsnoteApp.SpeechStateSpeechDecodingEncoding)
                        return qsTr("Processing, please wait...")
                    if (app.state === DsnoteApp.StateTranscribingFile)
                        return qsTr("Transcribing audio file...") +
                                (app.transcribe_progress > 0.0 ? " " +
                                                                 Math.round(app.transcribe_progress * 100) + "%" : "")
                    if (app.state === DsnoteApp.StateListeningSingleSentence ||
                            app.state === DsnoteApp.StateListeningAuto ||
                            app.state === DsnoteApp.StateListeningManual) return qsTr("Say something...")

                    if (app.state === DsnoteApp.StatePlayingSpeech) return qsTr("Reading a note...")

                    return ""
                }

                font.italic: true
                text: app.intermediate_text.length === 0 ? placeholderText : app.intermediate_text
                opacity: app.intermediate_text.length === 0 ? 0.6 : 1.0
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: appWin.padding
        Button {
            id: listenButton
            icon.name: "audio-input-microphone-symbolic"
            Layout.alignment: Qt.AlignVCenter
            Layout.fillWidth: true
            enabled: app.stt_configured &&
                     (app.state === DsnoteApp.StateIdle || app.state === DsnoteApp.StateListeningManual)
            text: qsTr("Listen")
            onPressed: {
                if (_settings.speech_mode === Settings.SpeechManual &&
                        app.state === DsnoteApp.StateIdle &&
                        app.speech === DsnoteApp.SpeechStateNoSpeech) {
                    app.listen()
                }
            }
            onClicked: {
                if (_settings.speech_mode !== Settings.SpeechManual &&
                        app.state === DsnoteApp.StateIdle &&
                        app.speech === DsnoteApp.SpeechStateNoSpeech)
                    app.listen()
            }
            onReleased: {
                if (app.state === DsnoteApp.StateListeningManual)
                    app.stop_listen()
            }
        }

        Button {
            icon.name: "audio-speakers-symbolic"
            Layout.alignment: Qt.AlignVCenter
            Layout.fillWidth: true
            enabled: app.tts_configured && textArea.text.length > 0 && app.state === DsnoteApp.StateIdle
            text: qsTr("Read")
            onClicked: app.play_speech()
        }

        Button {
            id: cancelButton
            Layout.alignment: Qt.AlignVCenter
            Layout.fillWidth: true
            icon.name: "action-unavailable-symbolic"
            enabled: app.speech === DsnoteApp.SpeechStateSpeechDecodingEncoding ||
                     app.speech === DsnoteApp.SpeechStateSpeechInitializing ||
                     app.state === DsnoteApp.StateTranscribingFile ||
                     app.state === DsnoteApp.StateListeningSingleSentence ||
                     app.state === DsnoteApp.StateListeningAuto ||
                     app.state === DsnoteApp.StatePlayingSpeech ||
                     app.state === DsnoteApp.StateWritingSpeechToFile
            text: qsTr("Cancel")
            onClicked: app.cancel()
        }
    }
}