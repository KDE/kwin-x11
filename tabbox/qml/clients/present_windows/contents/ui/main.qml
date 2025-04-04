/********************************************************************
 KWin - the KDE window manager
 This file is part of the KDE project.

Copyright (C) 2012 Martin Gräßlin <mgraesslin@kde.org>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*********************************************************************/
import QtQuick 2.0
import QtQuick.Layouts 1.1
import org.kde.plasma.core 2.0 as PlasmaCore
import org.kde.kquickcontrolsaddons 2.0
import org.kde.kwin 2.0 as KWin

KWin.Switcher {
    id: tabBox
    currentIndex: thumbnailListView.currentIndex

    PlasmaCore.Dialog {
        id: dialog
        location: PlasmaCore.Types.Floating
        visible: tabBox.visible
        flags: Qt.X11BypassWindowManagerHint
        x: tabBox.screenGeometry.x + tabBox.screenGeometry.width * 0.5 - dialogMainItem.width * 0.5
        y: tabBox.screenGeometry.y + tabBox.screenGeometry.height * 0.5 - dialogMainItem.height * 0.5

        mainItem: Item {
            id: dialogMainItem
            property int optimalWidth: 0.9*tabBox.screenGeometry.width
            property int optimalHeight: 0.9*tabBox.screenGeometry.height
            property int standardMargin: 2
            width: optimalWidth
            height: optimalHeight
            focus: true

            // just to get the margin sizes
            PlasmaCore.FrameSvgItem {
                id: hoverItem
                imagePath: "widgets/viewitem"
                prefix: "hover"
                visible: false
            }

            GridView {
                property int rows: Math.round(Math.sqrt(count))
                property int columns: (rows*rows < count) ? rows + 1 : rows
                id: thumbnailListView
                model: tabBox.model
                cellWidth: Math.floor(width / columns)
                cellHeight: Math.floor(height / rows)
                clip: true
                anchors {
                    fill: parent
                }
                delegate: Item {
                    width: thumbnailListView.cellWidth
                    height: thumbnailListView.cellHeight
                    Item {
                        id: thumbnailItem
                        anchors {
                            top: parent.top
                            left: parent.left
                            right: parent.right
                            bottom: captionItem.top
                            leftMargin: hoverItem.margins.left
                            rightMargin: hoverItem.margins.right
                            topMargin: hoverItem.margins.top
                            bottomMargin: dialogMainItem.standardMargin
                        }
                        KWin.ThumbnailItem {
                            wId: windowId
                            clip: true
                            clipTo: thumbnailListView
                            anchors.fill: parent
                        }
                    }
                    RowLayout {
                        id: captionItem
                        height: childrenRect.height
                        property int maximumWidth: parent.width - hoverItem.margins.left - dialogMainItem.standardMargin - hoverItem.margins.right
                        Layout.maximumWidth: captionItem.maximumWidth
                        anchors {
                            bottom: parent.bottom
                            bottomMargin: hoverItem.margins.bottom
                            horizontalCenter: parent.horizontalCenter
                        }
                        QIconItem {
                            id: iconItem
                            icon: model.icon
                            property int iconSize: 32
                            width: iconSize
                            height: iconSize
                            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                            Layout.preferredWidth: iconSize
                            Layout.preferredHeight: iconSize
                        }
                        Text {
                            text: caption
                            font.italic: minimized
                            font.bold: index == thumbnailListView.currentIndex
                            color: theme.textColor
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                            Layout.maximumWidth: captionItem.maximumWidth - iconItem.iconSize - captionItem.spacing * 2
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            thumbnailListView.currentIndex = index;
                        }
                    }
                }
                highlight: PlasmaCore.FrameSvgItem {
                    id: highlightItem
                    imagePath: "widgets/viewitem"
                    prefix: "hover"
                    width: thumbnailListView.cellWidth
                    height: thumbnailListView.cellHeight
                }
                boundsBehavior: Flickable.StopAtBounds
                Connections {
                    target: tabBox
                    onCurrentIndexChanged: {thumbnailListView.currentIndex = tabBox.currentIndex;}
                }
            }
            /*
            * Key navigation on outer item for two reasons:
            * @li we have to emit the change signal
            * @li on multiple invocation it does not work on the list view. Focus seems to be lost.
            **/
            Keys.onPressed: {
                if (event.key == Qt.Key_Left) {
                    thumbnailListView.moveCurrentIndexLeft();
                } else if (event.key == Qt.Key_Right) {
                    thumbnailListView.moveCurrentIndexRight();
                } else if (event.key == Qt.Key_Up) {
                    thumbnailListView.moveCurrentIndexUp();
                } else if (event.key == Qt.Key_Down) {
                    thumbnailListView.moveCurrentIndexDown();
                }
            }
        }
    }
}
