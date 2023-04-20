/* Copyright (C) 2021 Michal Kosciesza <michal@mkiol.net>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

import QtQuick 2.12
import QtQuick.Controls 2.5
import org.mkiol.dsnote.Dsnote 1.0

ApplicationWindow {
    id: appWin
    width: 640
    height: 480
    visible: true

    function push(page) {
        stackView.push(page)
    }

    header: ToolBar {
        contentHeight: toolButton.implicitHeight

        ToolButton {
            id: toolButton
            text: stackView.depth > 1 ? "\u25C0" : "\u2630"
            font.pixelSize: Qt.application.font.pixelSize * 1.6
            onClicked: {
                if (stackView.depth > 1) {
                    stackView.pop()
                } else {
                    drawer.open()
                }
            }
        }

        Label {
            text: stackView.currentItem.title
            anchors.centerIn: parent
        }
    }

    Drawer {
        id: drawer
        width: appWin.width * 0.66
        height: appWin.height

        Column {
            anchors.fill: parent

            ItemDelegate {
                text: qsTr("Settings")
                width: parent.width
                onClicked: {
                    stackView.push("SettingsPage.qml")
                    drawer.close()
                }
            }

            ItemDelegate {
                text: qsTr("Quit")
                width: parent.width
                onClicked: Qt.quit()
            }
        }
    }

    StackView {
        id: stackView
        initialItem: "NotesPage.qml"
        anchors.fill: parent
    }

    SttConfig {
        id: service
    }

    DsnoteApp {
        id: app

        onError: {
            switch (type) {
            case DsnoteApp.ErrorFileSource:
                console.log("File source error")
                break;
            case DsnoteApp.ErrorMicSource:
                console.log("Mic source error")
                break;
            default:
                console.log("Unknown error")
            }
        }
    }
}
