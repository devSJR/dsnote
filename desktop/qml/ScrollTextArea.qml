/* Copyright (C) 2023 Michal Kosciesza <michal@mkiol.net>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

import QtQuick 2.12
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.3

Item {
    id: root

    property alias textArea: _textArea
    property bool canUndo: true
    property bool canUndoFallback: false
    property bool canRedo: true
    property bool canClear: true
    property bool canPaste: true
    readonly property bool _fitContent: scrollView.availableHeight - 2 * appWin.padding >= scrollView.contentHeight
    readonly property bool _contextActive: copyButton.hovered || clearButton.hovered ||
                                           undoButton.hovered || redoButton.hovered || pasteButton.hovered ||
                                           _fitContent

    signal clearClicked()
    signal copyClicked()
    signal undoFallbackClicked()

    function scrollToBottom() {
        var position = (scrollView.contentHeight - scrollView.availableHeight) / scrollView.contentHeight
        if (position > 0)
            scrollView._bar.position = position
    }

    ScrollView {
        id: scrollView

        property ScrollBar _bar: ScrollBar.vertical

        anchors.fill: parent
        enabled: root.enabled
        clip: true

        opacity: enabled ? root._contextActive && !root._fitContent ? 0.4 : 1.0 : 0.0
        Behavior on opacity { OpacityAnimator { duration: 100 } }

        TextArea {
            id: _textArea

            wrapMode: TextEdit.WordWrap
            verticalAlignment: TextEdit.AlignTop

            Keys.onUpPressed: _bar.decrease()
            Keys.onDownPressed: _bar.increase()
        }
    }

    RowLayout {
        opacity: root._contextActive ? 1.0 : 0.4
        Behavior on opacity { OpacityAnimator { duration: 100 } }
        visible: opacity > 0.0 && root.textArea.enabled

        anchors {
            bottom: parent.bottom
            bottomMargin: appWin.padding
            right: parent.right
            rightMargin: appWin.padding + (scrollView._bar.visible ? scrollView._bar.width : 0)
        }

        ToolButton {
            id: copyButton

            visible: root.textArea.text.length !== 0
            icon.name: "edit-copy-symbolic"
            onClicked: root.copyClicked()

            ToolTip.visible: hovered
            ToolTip.delay: Qt.styleHints.mousePressAndHoldInterval
            ToolTip.text: qsTr("Copy")
        }
        ToolButton {
            id: pasteButton

            visible: root.canPaste && root.textArea.canPaste
            icon.name: "edit-paste-symbolic"
            onClicked: root.textArea.paste()

            ToolTip.visible: hovered
            ToolTip.delay: Qt.styleHints.mousePressAndHoldInterval
            ToolTip.text: qsTr("Paste")
        }
        ToolButton {
            id: clearButton

            visible: root.canClear && !root.textArea.readOnly && root.textArea.text.length !== 0
            icon.name: "edit-clear-all-symbolic"
            onClicked: root.clearClicked()

            ToolTip.visible: hovered
            ToolTip.delay: Qt.styleHints.mousePressAndHoldInterval
            ToolTip.text: qsTr("Clear")
        }
        ToolButton {
            id: undoButton

            visible: !root.textArea.readOnly && root.canUndo && (root.textArea.canUndo || root.canUndoFallback)
            icon.name: "edit-undo-symbolic"
            onClicked: {
                if (root.textArea.canUndo)
                    root.textArea.undo()
                else
                    root.undoFallbackClicked()
            }

            ToolTip.visible: hovered
            ToolTip.delay: Qt.styleHints.mousePressAndHoldInterval
            ToolTip.text: qsTr("Undo")
        }
        ToolButton {
            id: redoButton

            visible: !root.textArea.readOnly && root.canRedo && root.textArea.canRedo
            icon.name: "edit-redo-symbolic"
            onClicked: root.textArea.redo()

            ToolTip.visible: hovered
            ToolTip.delay: Qt.styleHints.mousePressAndHoldInterval
            ToolTip.text: qsTr("Redo")
        }
    }
}
