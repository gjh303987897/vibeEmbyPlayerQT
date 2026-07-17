import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import VibePlayer 1.0

ApplicationWindow {
    id: root
    width: 1240
    height: 780
    minimumWidth: 980
    minimumHeight: 640
    visible: false
    title: qsTr("vibePlayerQT")
    color: theme.bg

    property int pendingDeleteRow: -1
    property int pendingScheduledDeleteRow: -1
    property int dragFromRow: -1
    property bool playerImmersive: false
    property string downloadWarningTitle: ""
    property string downloadWarningMessage: ""
    property bool darkTheme: appViewModel.effectiveTheme !== "light"
    property var theme: darkTheme ? dark : light
    property var dark: ({
        bg: "#0f1217",
        surface: "#171c22",
        elevated: "#1d232b",
        elevatedHover: "#252d36",
        input: "#121820",
        text: "#f4f7fb",
        muted: "#9aa7b5",
        subtle: "#6f7b89",
        border: "#303945",
        primary: "#4f8cff",
        primaryHover: "#6aa0ff",
        danger: "#f45f74",
        success: "#72d88f",
        warning: "#f0b46b",
        errorBg: "#3a2026",
        errorText: "#ffdce3",
        shadow: "#66000000"
    })
    property var light: ({
        bg: "#f5f7fb",
        surface: "#ffffff",
        elevated: "#ffffff",
        elevatedHover: "#f1f5fb",
        input: "#ffffff",
        text: "#151922",
        muted: "#5d6978",
        subtle: "#8792a1",
        border: "#d8e0ea",
        primary: "#1677ff",
        primaryHover: "#4096ff",
        danger: "#d9363e",
        success: "#389e0d",
        warning: "#d48806",
        errorBg: "#fff1f0",
        errorText: "#a8071a",
        shadow: "#22000000"
    })

    function t(key) {
        appViewModel.translationRevision
        return appViewModel.trText(key)
    }

    function formatDuration(seconds) {
        if (!Number.isFinite(seconds) || seconds <= 0) {
            return "0m"
        }
        var total = Math.floor(seconds)
        var hours = Math.floor(total / 3600)
        var minutes = Math.floor((total % 3600) / 60)
        if (hours <= 0) {
            return Math.max(1, minutes) + "m"
        }
        if (minutes <= 0) {
            return hours + "h"
        }
        return hours + "h " + minutes + "m"
    }

    function formatBytes(bytes) {
        if (!Number.isFinite(bytes) || bytes <= 0) {
            return "0 B"
        }
        var value = Number(bytes)
        var units = ["B", "KB", "MB", "GB", "TB"]
        var unit = 0
        while (value >= 1024 && unit < units.length - 1) {
            value = value / 1024
            unit += 1
        }
        return unit === 0 ? Math.round(value) + " " + units[unit] : value.toFixed(value >= 10 ? 1 : 2) + " " + units[unit]
    }

    function formatTrafficSplit(bytesIn, bytesOut) {
        return "↓ " + formatBytes(bytesIn) + "  ·  ↑ " + formatBytes(bytesOut)
    }

    function withAlpha(value, alpha) {
        return Qt.rgba(value.r, value.g, value.b, alpha)
    }

    function serviceAccentColor(serviceType) {
        switch (String(serviceType).toLowerCase()) {
        case "emby":
            return Qt.rgba(0.322, 0.710, 0.294, 1.0)
        case "jellyfin":
            return Qt.rgba(0.608, 0.427, 1.0, 1.0)
        case "webdav":
            return Qt.rgba(0.184, 0.561, 1.0, 1.0)
        case "iptv":
            return Qt.rgba(1.0, 0.478, 0.239, 1.0)
        default:
            return Qt.rgba(0.392, 0.455, 0.545, 1.0)
        }
    }

    function formatHistoryDate(value) {
        if (!value || value.length < 10) {
            return value
        }
        return value.substring(5, 10)
    }

    function enterPlayerFullscreen() {
        playerImmersive = true
        root.showFullScreen()
    }

    function exitPlayerFullscreen() {
        playerImmersive = false
        if (root.visibility === Window.FullScreen) {
            root.showNormal()
        }
    }

    Component.onCompleted: {
        trayController.attachWindow(root)
        windowAppearanceController.attachWindow(root)
        windowAppearanceController.applyTheme(appViewModel.effectiveTheme)
        root.visible = true
        appViewModel.initialize()
    }

    onClosing: function(close) {
        if (appViewModel.minimizeToTray && trayController.trayAvailable) {
            close.accepted = false
            trayController.hideToTray()
        }
    }

    Connections {
        target: appViewModel

        function onCurrentViewChanged() {
            if (appViewModel.currentView !== "player" && root.playerImmersive) {
                root.exitPlayerFullscreen()
            }
        }

        function onCertificatePromptRequested(host, details) {
            certificateDialog.host = host
            certificateDialog.details = details
            certificateDialog.open()
        }

        function onPasswordRequired(serviceName, username) {
            passwordDialog.serviceName = serviceName
            passwordDialog.username = username
            passwordDialog.password = ""
            passwordDialog.open()
        }

        function onTranslationsChanged() {
            root.title = t("app.title")
        }

        function onEffectiveThemeChanged() {
            windowAppearanceController.applyTheme(appViewModel.effectiveTheme)
        }

        function onPageTransitionsEnabledChanged() {
            if (!appViewModel.pageTransitionsEnabled) {
                pageStack.resetTransition()
            }
        }

        function onDownloadSpaceWarningRequested(title, message) {
            root.downloadWarningTitle = title
            root.downloadWarningMessage = message
            downloadWarningDialog.open()
        }

        function onMissedScheduledPlaybackTasksChanged() {
            if (appViewModel.missedScheduledPlaybackPromptVisible) {
                if (!missedScheduleNotification.visible) {
                    missedScheduleNotification.open()
                }
            } else if (missedScheduleNotification.visible) {
                missedScheduleNotification.close()
            }
        }
    }

    Popup {
        id: missedScheduleNotification
        readonly property real safeMargin: 24
        readonly property real naturalHeight: notificationContent.implicitHeight + topPadding + bottomPadding

        parent: Overlay.overlay
        width: Math.min(440, parent.width - safeMargin * 2)
        height: Math.min(naturalHeight, parent.height - safeMargin * 2)
        x: Math.round(Math.max(safeMargin, parent.width - width - safeMargin))
        y: Math.round(Math.max(safeMargin, parent.height - height - safeMargin))
        margins: safeMargin
        padding: 18
        clip: true
        modal: false
        focus: false
        closePolicy: Popup.NoAutoClose
        transformOrigin: Item.BottomRight
        z: 1000

        enter: Transition {
            ParallelAnimation {
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 180; easing.type: Easing.OutCubic }
                NumberAnimation { property: "scale"; from: 0.96; to: 1; duration: 220; easing.type: Easing.OutBack }
            }
        }

        exit: Transition {
            ParallelAnimation {
                NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 130; easing.type: Easing.InCubic }
                NumberAnimation { property: "scale"; from: 1; to: 0.98; duration: 130; easing.type: Easing.InCubic }
            }
        }

        background: Rectangle {
            radius: 14
            color: theme.surface
            border.width: 1
            border.color: root.withAlpha(theme.warning, 0.72)

            Rectangle {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: 4
                radius: 2
                color: theme.warning
            }
        }

        contentItem: ColumnLayout {
            id: notificationContent
            spacing: 14

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Rectangle {
                    Layout.preferredWidth: 36
                    Layout.preferredHeight: 36
                    radius: 18
                    color: root.withAlpha(theme.warning, 0.16)
                    border.color: root.withAlpha(theme.warning, 0.5)

                    Label {
                        anchors.centerIn: parent
                        text: "!"
                        color: theme.warning
                        font.pixelSize: 19
                        font.bold: true
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2

                    Label {
                        Layout.fillWidth: true
                        text: t("schedule.missedTitle")
                        color: theme.text
                        font.pixelSize: 16
                        font.bold: true
                        elide: Text.ElideRight
                    }

                    MutedText {
                        Layout.fillWidth: true
                        text: appViewModel.missedScheduledPlaybackTaskCount + " " + t("nav.scheduledTasks")
                        elide: Text.ElideRight
                    }
                }
            }

            BodyText {
                Layout.fillWidth: true
                text: appViewModel.missedScheduledPlaybackMessage
                wrapMode: Text.WordWrap
                maximumLineCount: 4
                elide: Text.ElideRight
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                Item { Layout.fillWidth: true }

                ModernButton {
                    text: t("schedule.missedIgnore")
                    onClicked: appViewModel.resolveMissedScheduledPlaybackTasks(false)
                }

                ModernButton {
                    text: t("schedule.missedRun")
                    onClicked: appViewModel.resolveMissedScheduledPlaybackTasks(true)
                }
            }
        }
    }

    ModernDialog {
        id: certificateDialog
        property string host: ""
        property string details: ""
        title: t("dialog.certificateTitle")
        standardButtons: Dialog.Yes | Dialog.No
        width: Math.min(root.width - 64, 560)

        ColumnLayout {
            spacing: 12
            width: parent.width

            BodyText {
                Layout.fillWidth: true
                text: t("dialog.certificatePrefix") + certificateDialog.host + t("dialog.certificateSuffix")
                wrapMode: Text.WordWrap
            }

            MutedText {
                Layout.fillWidth: true
                text: certificateDialog.details
                wrapMode: Text.WordWrap
            }
        }

        onAccepted: appViewModel.acceptPendingCertificate(true)
        onRejected: appViewModel.acceptPendingCertificate(false)
    }

    ModernDialog {
        id: passwordDialog
        property string serviceName: ""
        property string username: ""
        property string password: ""
        title: t("dialog.passwordTitle")
        standardButtons: Dialog.Ok | Dialog.Cancel
        width: Math.min(root.width - 64, 420)

        ColumnLayout {
            width: parent.width
            spacing: 12

            BodyText {
                Layout.fillWidth: true
                text: passwordDialog.serviceName + " · " + passwordDialog.username
                elide: Text.ElideRight
            }

            ModernTextField {
                id: passwordField
                Layout.fillWidth: true
                placeholderText: t("form.password")
                echoMode: TextInput.Password
                text: passwordDialog.password
                onTextChanged: passwordDialog.password = text
                onAccepted: passwordDialog.accept()
            }
        }

        onOpened: passwordField.forceActiveFocus()
        onAccepted: appViewModel.loginSelectedService(passwordDialog.password)
    }

    ModernDialog {
        id: privacyPinDialog
        property string pin: ""
        title: t("privacy.pinTitle")
        standardButtons: Dialog.Ok | Dialog.Cancel
        width: Math.min(root.width - 64, 380)

        PinEntryField {
            id: privacyPinField
            width: parent.width
            placeholderText: t("privacy.pinPlaceholder")
            text: privacyPinDialog.pin
            onTextChanged: privacyPinDialog.pin = text
            onAccepted: privacyPinDialog.accept()
        }

        onOpened: privacyPinField.forceActiveFocus()
        onAccepted: {
            appViewModel.unlockPrivacyMode(privacyPinDialog.pin)
            privacyPinDialog.pin = ""
        }
        onRejected: privacyPinDialog.pin = ""
    }

    ModernDialog {
        id: privacyCardsDialog
        title: t("privacy.editorTitle")
        standardButtons: Dialog.Ok
        width: Math.min(root.width - 64, 620)
        height: Math.min(root.height - 96, 560)

        ColumnLayout {
            width: parent.width
            spacing: 12

            MutedText {
                Layout.fillWidth: true
                text: t("privacy.editorHint")
                wrapMode: Text.WordWrap
            }

            ListView {
                id: privacyCardsList
                Layout.fillWidth: true
                Layout.preferredHeight: Math.max(120, Math.min(380, appViewModel.privacyCards.count * 62 + 6))
                clip: true
                spacing: 8
                model: appViewModel.privacyCards

                delegate: Rectangle {
                    width: privacyCardsList.width
                    height: 56
                    radius: 10
                    color: privacyRowMouse.containsMouse ? theme.elevatedHover : theme.elevated
                    border.color: theme.border

                    MouseArea {
                        id: privacyRowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: privacyCardCheck.checked = !privacyCardCheck.checked
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 12

                        ModernCheckBox {
                            id: privacyCardCheck
                            checked: model.privateMode
                            onToggled: appViewModel.setPrivacyCardPrivate(index, checked)
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            Label {
                                Layout.fillWidth: true
                                text: model.name
                                color: theme.text
                                font.pixelSize: 15
                                font.bold: true
                                elide: Text.ElideRight
                            }

                            MutedText {
                                Layout.fillWidth: true
                                text: model.serviceType + " 路 " + (model.host.length > 0 ? model.host : model.baseUrl)
                                elide: Text.ElideRight
                            }
                        }
                    }
                }
            }
        }

        onOpened: appViewModel.refreshPrivacyCards()
    }

    ModernDialog {
        id: serviceDialog
        title: t("dialog.serviceTitle")
        standardButtons: Dialog.Save | Dialog.Cancel
        width: Math.min(root.width - 64, 540)

        ColumnLayout {
            width: parent.width
            spacing: 12

            ModernComboBox {
                Layout.fillWidth: true
                model: ["Emby", "Jellyfin", "IPTV", "WebDAV"]
                currentIndex: appViewModel.serviceType === "WebDAV" ? 3 : appViewModel.serviceType === "IPTV" ? 2 : appViewModel.serviceType === "Jellyfin" ? 1 : 0
                onActivated: appViewModel.serviceType = currentText
            }

            ModernTextField {
                Layout.fillWidth: true
                placeholderText: t("form.serviceName")
                text: appViewModel.serverName
                onTextChanged: appViewModel.serverName = text
            }

            ModernTextField {
                Layout.fillWidth: true
                visible: appViewModel.serviceType !== "IPTV"
                placeholderText: appViewModel.serviceType === "WebDAV" ? t("form.webDavEndpoint") : t("form.serverUrl")
                inputMethodHints: Qt.ImhUrlCharactersOnly
                text: appViewModel.serverUrl
                onTextChanged: appViewModel.serverUrl = text
            }

            ModernTextField {
                Layout.fillWidth: true
                visible: appViewModel.serviceType !== "IPTV"
                placeholderText: t("form.username")
                text: appViewModel.username
                onTextChanged: appViewModel.username = text
            }

            ModernTextField {
                Layout.fillWidth: true
                visible: appViewModel.serviceType !== "IPTV"
                placeholderText: t("form.password")
                echoMode: TextInput.Password
                text: appViewModel.password
                onTextChanged: appViewModel.password = text
            }

            RowLayout {
                Layout.fillWidth: true
                visible: appViewModel.serviceType === "IPTV"
                spacing: 10

                ModernTextField {
                    Layout.fillWidth: true
                    readOnly: true
                    placeholderText: t("iptv.filePlaceholder")
                    text: appViewModel.iptvFilePath
                }

                ModernButton {
                    text: t("iptv.chooseFile")
                    onClicked: appViewModel.chooseIptvPlaylistFile()
                }
            }

            ModernCheckBox {
                visible: appViewModel.serviceType !== "IPTV"
                text: t("form.autoLogin")
                checked: appViewModel.autoLogin
                onToggled: appViewModel.autoLogin = checked
            }

            ModernCheckBox {
                visible: appViewModel.serviceType !== "IPTV"
                text: t("form.selfSigned")
                checked: appViewModel.trustSelfSignedCertificate
                onToggled: appViewModel.trustSelfSignedCertificate = checked
            }
        }

        onAccepted: appViewModel.saveServiceCard()
    }

    ModernDialog {
        id: downloadWarningDialog
        title: root.downloadWarningTitle
        standardButtons: Dialog.Ok | Dialog.Cancel
        width: Math.min(root.width - 64, 520)

        BodyText {
            width: parent.width
            text: root.downloadWarningMessage
            wrapMode: Text.WordWrap
        }

        onAccepted: appViewModel.acceptPendingDownloadWarning(true)
        onRejected: appViewModel.acceptPendingDownloadWarning(false)
    }

    ModernDialog {
        id: deleteDialog
        title: t("dialog.deleteTitle")
        standardButtons: Dialog.Ok | Dialog.Cancel
        width: Math.min(root.width - 64, 460)

        ColumnLayout {
            width: parent.width
            spacing: 12

            BodyText {
                Layout.fillWidth: true
                text: t("dialog.deletePrompt")
                wrapMode: Text.WordWrap
            }

            ModernCheckBox {
                id: deleteLocalDataCheck
                text: t("dialog.deleteLocalData")
                checked: true
            }
        }

        onAccepted: {
            appViewModel.deleteServiceCard(root.pendingDeleteRow, deleteLocalDataCheck.checked)
            root.pendingDeleteRow = -1
        }
        onRejected: root.pendingDeleteRow = -1
    }

    ModernDialog {
        id: scheduledTaskEditorDialog
        property bool editing: false
        title: editing ? t("schedule.edit") : t("schedule.add")
        standardButtons: Dialog.Cancel
        width: Math.min(root.width - 64, 680)

        Flickable {
            id: scheduledTaskEditorFlick
            width: parent.width
            implicitHeight: Math.min(editorColumn.implicitHeight, Math.max(300, root.height - 230))
            contentWidth: width
            contentHeight: editorColumn.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            ColumnLayout {
                id: editorColumn
                width: scheduledTaskEditorFlick.width - 14
                spacing: 12

            SettingRow {
                label: t("schedule.source")
                ModernComboBox {
                    Layout.preferredWidth: 270
                    model: appViewModel.scheduledEmbySources
                    textRole: "name"
                    currentIndex: appViewModel.scheduledTaskSourceIndex
                    enabled: count > 0
                    onActivated: appViewModel.scheduledTaskSourceIndex = index
                }
            }

            SettingRow {
                label: t("schedule.type")
                ModernComboBox {
                    Layout.preferredWidth: 360
                    textRole: "label"
                    valueRole: "value"
                    model: [
                        { label: t("schedule.typeManual"), value: "manual" },
                        { label: t("schedule.typeDaily"), value: "daily" },
                        { label: t("schedule.typeWeekly"), value: "weekly" },
                        { label: t("schedule.typeMonthly"), value: "monthly" },
                        { label: t("schedule.typeCustomMonthly"), value: "custom_monthly" }
                    ]
                    currentIndex: appViewModel.scheduledTaskScheduleType === "manual" ? 0
                        : appViewModel.scheduledTaskScheduleType === "daily" ? 1
                        : appViewModel.scheduledTaskScheduleType === "weekly" ? 2
                        : appViewModel.scheduledTaskScheduleType === "monthly" ? 3 : 4
                    onActivated: appViewModel.scheduledTaskScheduleType = model[index].value
                }
            }

            SettingRow {
                visible: appViewModel.scheduledTaskScheduleType !== "manual"
                label: t("schedule.startTime")
                RowLayout {
                    Layout.preferredWidth: 360
                    spacing: 8

                    ModernSpinBox {
                        from: 0
                        to: 23
                        value: appViewModel.scheduledTaskStartHour
                        editable: true
                        textFromValue: function(value, locale) {
                            return value < 10 ? "0" + value : value.toString()
                        }
                        onValueModified: appViewModel.scheduledTaskStartHour = value
                    }

                    Label {
                        text: ":"
                        color: theme.text
                        font.pixelSize: 17
                        font.bold: true
                    }

                    ModernSpinBox {
                        from: 0
                        to: 59
                        stepSize: 5
                        value: appViewModel.scheduledTaskStartMinute
                        editable: true
                        textFromValue: function(value, locale) {
                            return value < 10 ? "0" + value : value.toString()
                        }
                        onValueModified: appViewModel.scheduledTaskStartMinute = value
                    }
                    Item { Layout.fillWidth: true }
                }
            }

            SettingRow {
                visible: appViewModel.scheduledTaskScheduleType === "weekly"
                label: t("schedule.weekday")
                ModernComboBox {
                    Layout.preferredWidth: 360
                    model: [
                        t("schedule.weekday1"), t("schedule.weekday2"), t("schedule.weekday3"),
                        t("schedule.weekday4"), t("schedule.weekday5"), t("schedule.weekday6"),
                        t("schedule.weekday7")
                    ]
                    currentIndex: appViewModel.scheduledTaskWeekday - 1
                    onActivated: appViewModel.scheduledTaskWeekday = index + 1
                }
            }

            SettingRow {
                visible: appViewModel.scheduledTaskScheduleType === "monthly"
                label: t("schedule.monthDay")
                RowLayout {
                    Layout.preferredWidth: 360
                    ModernSpinBox {
                        from: 1
                        to: 31
                        value: appViewModel.scheduledTaskMonthDay
                        editable: true
                        onValueModified: appViewModel.scheduledTaskMonthDay = value
                    }
                    Item { Layout.fillWidth: true }
                }
            }

            SettingRow {
                visible: appViewModel.scheduledTaskScheduleType === "custom_monthly"
                label: t("schedule.customDays")

                GridLayout {
                    Layout.preferredWidth: 360
                    columns: 7
                    columnSpacing: 7
                    rowSpacing: 7

                    Repeater {
                        model: 31

                        delegate: Button {
                            id: dayButton
                            property bool selected: appViewModel.scheduledTaskCustomMonthDays.indexOf(index + 1) >= 0
                            Layout.preferredWidth: 42
                            Layout.preferredHeight: 34
                            text: (index + 1).toString()
                            hoverEnabled: true
                            onClicked: appViewModel.toggleScheduledTaskCustomMonthDay(index + 1)

                            contentItem: Label {
                                text: dayButton.text
                                color: dayButton.selected ? "#ffffff" : theme.text
                                font.pixelSize: 13
                                font.bold: dayButton.selected
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }

                            background: Rectangle {
                                radius: 8
                                color: dayButton.selected ? theme.primary
                                    : dayButton.hovered ? theme.elevatedHover : theme.input
                                border.color: dayButton.selected ? theme.primary : theme.border
                            }
                        }
                    }
                }
            }

            SettingRow {
                label: t("schedule.duration")
                RowLayout {
                    Layout.preferredWidth: 360
                    spacing: 10
                    ModernSpinBox {
                        from: 5
                        to: 720
                        stepSize: 5
                        value: appViewModel.scheduledTaskDurationMinutes
                        onValueModified: appViewModel.scheduledTaskDurationMinutes = value
                    }
                    MutedText { text: t("schedule.minutes") }
                    Item { Layout.fillWidth: true }
                }
            }

            SettingRow {
                visible: appViewModel.scheduledTaskScheduleType !== "manual"
                label: t("schedule.enabled")
                RowLayout {
                    Layout.preferredWidth: 360
                    ModernCheckBox {
                        checked: appViewModel.scheduledTaskEnabled
                        onToggled: appViewModel.scheduledTaskEnabled = checked
                    }
                    Item { Layout.fillWidth: true }
                }
            }

            MutedText {
                Layout.fillWidth: true
                visible: appViewModel.scheduledEmbySources.count === 0
                text: t("schedule.noSources")
                color: theme.warning
                wrapMode: Text.WordWrap
            }

            MutedText {
                Layout.fillWidth: true
                visible: appViewModel.scheduledEmbySources.count > 0
                text: appViewModel.scheduledTaskScheduleType === "manual"
                    ? t("schedule.manualHint") : t("schedule.scheduledHint")
                wrapMode: Text.WordWrap
            }

                RowLayout {
                    Layout.fillWidth: true
                    Item { Layout.fillWidth: true }
                    ModernButton {
                        text: t("schedule.save")
                        enabled: appViewModel.scheduledEmbySources.count > 0
                        onClicked: {
                            if (appViewModel.saveScheduledPlaybackTask()) {
                                scheduledTaskEditorDialog.close()
                            }
                        }
                    }
                    ModernButton {
                        text: t("schedule.saveAndRun")
                        enabled: appViewModel.scheduledEmbySources.count > 0
                            && !appViewModel.scheduledPlaybackActive
                            && !appViewModel.scheduledPlaybackWaiting
                        onClicked: {
                            if (appViewModel.saveAndRunScheduledPlaybackTask()) {
                                scheduledTaskEditorDialog.close()
                            }
                        }
                    }
                }
            }
        }
    }

    ModernDialog {
        id: scheduledTaskDeleteDialog
        title: t("schedule.deleteTitle")
        standardButtons: Dialog.Ok | Dialog.Cancel
        width: Math.min(root.width - 64, 440)

        BodyText {
            width: parent.width
            text: t("schedule.deletePrompt")
            wrapMode: Text.WordWrap
        }

        onAccepted: {
            appViewModel.deleteScheduledPlaybackTask(root.pendingScheduledDeleteRow)
            root.pendingScheduledDeleteRow = -1
        }
        onRejected: root.pendingScheduledDeleteRow = -1
    }

    Dialog {
        id: overviewDialog
        modal: true
        anchors.centerIn: parent
        padding: 0
        width: Math.min(root.width - 64, 680)
        height: Math.min(root.height - 88, 560)

        background: Rectangle {
            color: "#0b0f15"
            radius: 14
            border.color: "#27313d"
            border.width: 1
        }

        contentItem: ColumnLayout {
            spacing: 0

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 82
                radius: 14
                color: "#0b0f15"

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 1
                    color: "#1f2a36"
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 24
                    anchors.rightMargin: 18
                    spacing: 12

                    Label {
                        Layout.fillWidth: true
                        text: t("dialog.overviewTitle")
                        color: "#ffffff"
                        font.pixelSize: 28
                        font.bold: true
                        elide: Text.ElideRight
                    }

                    Button {
                        id: overviewHeaderCloseButton
                        implicitWidth: 38
                        implicitHeight: 38
                        text: "×"
                        font.pixelSize: 22
                        font.bold: true
                        onClicked: overviewDialog.close()

                        contentItem: Label {
                            text: overviewHeaderCloseButton.text
                            color: overviewHeaderCloseButton.enabled ? "#ffffff" : "#6d7784"
                            font: overviewHeaderCloseButton.font
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        background: Rectangle {
                            radius: 19
                            color: overviewHeaderCloseButton.down ? "#2d7dff"
                                : overviewHeaderCloseButton.hovered ? "#1c2633"
                                : "#111820"
                            border.color: overviewHeaderCloseButton.hovered ? "#4f8cff" : "#2f3b48"
                        }
                    }
                }
            }

            ScrollView {
                id: overviewScroll
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                Layout.topMargin: 18
                Layout.bottomMargin: 18
                clip: true

                BodyText {
                    width: overviewScroll.availableWidth
                    text: appViewModel.selectedItemOverview.length > 0 ? appViewModel.selectedItemOverview : t("details.noOverview")
                    color: "#ffffff"
                    font.pixelSize: 15
                    wrapMode: Text.WordWrap
                    lineHeight: 1.18
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 76
                radius: 14
                color: "#0b0f15"

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: 1
                    color: "#1f2a36"
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 24
                    anchors.rightMargin: 24
                    spacing: 12

                    Item { Layout.fillWidth: true }

                    Button {
                        id: overviewCloseButton
                        implicitHeight: 40
                        leftPadding: 22
                        rightPadding: 22
                        text: t("action.dismiss")
                        font.pixelSize: 14
                        font.bold: true
                        onClicked: overviewDialog.close()

                        contentItem: Label {
                            text: overviewCloseButton.text
                            color: "#ffffff"
                            font: overviewCloseButton.font
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        background: Rectangle {
                            radius: 9
                            color: overviewCloseButton.down ? "#2d7dff"
                                : overviewCloseButton.hovered ? "#1f6fff"
                                : "#1677ff"
                            border.color: overviewCloseButton.hovered ? "#73a7ff" : "#1677ff"
                        }
                    }
                }
            }
        }
    }

    header: ToolBar {
        height: root.playerImmersive ? 0 : 64
        visible: !root.playerImmersive
        enabled: !root.playerImmersive
        background: Rectangle {
            color: theme.surface
            border.color: theme.border
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 24
            anchors.rightMargin: 24
            spacing: 12

            IconButton {
                text: "‹"
                visible: appViewModel.currentView !== "services" && appViewModel.currentView !== "settings"
                font.pixelSize: 28
                onClicked: {
                    if (appViewModel.currentView === "history") {
                        appViewModel.backToServices()
                    } else if (appViewModel.currentView === "scheduledTasks") {
                        appViewModel.backToServices()
                    } else if (appViewModel.currentView === "webdav") {
                        appViewModel.webDavBack()
                    } else if (appViewModel.currentView === "search") {
                        appViewModel.clearServerSearch()
                    } else if (appViewModel.currentView === "library") {
                        appViewModel.mediaLibraryBack()
                    } else if (appViewModel.currentView === "details") {
                        appViewModel.mediaDetailsBack()
                    } else if (appViewModel.currentView === "home") {
                        appViewModel.backToServices()
                    } else {
                        appViewModel.backToHome()
                    }
                }
            }

            ColumnLayout {
                spacing: 0
                Layout.fillWidth: true

                RowLayout {
                    id: pageTitleRow
                    Layout.fillWidth: true
                    spacing: 8
                    property real privacyBadgeWidth: 76

                    Label {
                        Layout.maximumWidth: Math.max(0, pageTitleRow.width
                            - (privacyModeBadge.visible ? pageTitleRow.privacyBadgeWidth + pageTitleRow.spacing : 0))
                        text: appViewModel.currentView === "settings" ? t("settings.title")
                            : appViewModel.currentView === "history" ? t("history.title")
                            : appViewModel.currentView === "scheduledTasks" ? t("nav.scheduledTasks")
                            : appViewModel.currentView === "services" ? t("nav.services")
                                : appViewModel.currentServerName
                        color: theme.text
                        font.pixelSize: 20
                        font.bold: true
                        elide: Text.ElideRight
                    }

                    Rectangle {
                        id: privacyModeBadge
                        visible: appViewModel.currentView === "services" && appViewModel.privacyMode
                        Layout.preferredHeight: 24
                        Layout.preferredWidth: pageTitleRow.privacyBadgeWidth
                        radius: 8
                        color: theme.primary
                        border.color: theme.primary

                        Label {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            text: t("history.privateBadge")
                            color: "#ffffff"
                            font.pixelSize: 11
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                    }

                    Item { Layout.fillWidth: true }
                }

                MutedText {
                    Layout.fillWidth: true
                    text: appViewModel.currentView === "settings" ? t("settings.subtitle")
                        : appViewModel.currentView === "history" ? (appViewModel.privacyMode ? t("history.subtitlePrivacy") : t("history.subtitle"))
                        : appViewModel.currentView === "scheduledTasks" ? t("schedule.subtitle")
                        : appViewModel.currentView === "iptv" ? appViewModel.currentUser
                        : appViewModel.loggedIn ? appViewModel.currentUser
                        : t("nav.chooseSource")
                    elide: Text.ElideRight
                }
            }

            BusyIndicator {
                running: appViewModel.loading
                visible: appViewModel.loading
                implicitWidth: 28
                implicitHeight: 28
            }

            MediaServerSearchBar {
                visible: appViewModel.serverSearchAvailable
                    && (appViewModel.currentView === "home" || appViewModel.currentView === "search")
                Layout.minimumWidth: visible ? 300 : 0
                Layout.preferredWidth: visible ? Math.min(380, Math.max(320, root.width * 0.30)) : 0
            }

            IconButton {
                text: appViewModel.privacyMode ? "\uD83D\uDD13" : "\uD83D\uDD12"
                visible: appViewModel.currentView === "services"
                ToolTip.visible: hovered
                ToolTip.text: t("nav.privacy")
                onClicked: {
                    if (appViewModel.privacyMode) {
                        appViewModel.exitPrivacyMode()
                    } else if (appViewModel.privacyPinConfigured) {
                        privacyPinDialog.open()
                    } else {
                        appViewModel.unlockPrivacyMode("")
                        appViewModel.openSettings()
                    }
                }
            }

            ModernButton {
                text: t("privacy.editCards")
                visible: appViewModel.currentView === "services" && appViewModel.privacyMode
                onClicked: {
                    appViewModel.refreshPrivacyCards()
                    privacyCardsDialog.open()
                }
            }

            ModernButton {
                text: t("nav.scheduledTasks")
                visible: appViewModel.currentView === "services"
                onClicked: appViewModel.openScheduledPlaybackTasks()
            }

            ModernButton {
                text: t("nav.history")
                visible: appViewModel.currentView === "services"
                onClicked: appViewModel.openHistoryStats()
            }

            ModernButton {
                text: t("action.add")
                visible: appViewModel.currentView === "services"
                onClicked: {
                    appViewModel.editingServices = false
                    appViewModel.beginAddServiceCard()
                    serviceDialog.open()
                }
            }

            ModernButton {
                text: appViewModel.editingServices ? t("action.done") : t("action.edit")
                visible: appViewModel.currentView === "services"
                onClicked: appViewModel.editingServices = !appViewModel.editingServices
            }

            ModernButton {
                text: t("action.refresh")
                visible: appViewModel.currentView === "home"
                enabled: !appViewModel.loading
                onClicked: appViewModel.refreshHome()
            }

            ModernButton {
                text: t("nav.settings")
                visible: appViewModel.currentView !== "settings"
                onClicked: appViewModel.openSettings()
            }

            ModernButton {
                text: t("action.backToServices")
                visible: appViewModel.currentView !== "services"
                onClicked: appViewModel.backToServices()
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: theme.bg

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: root.playerImmersive ? 0 : 26
            spacing: root.playerImmersive ? 0 : 16

            Rectangle {
                visible: appViewModel.errorMessage.length > 0 && !root.playerImmersive
                Layout.fillWidth: true
                radius: 8
                color: theme.errorBg
                border.color: theme.danger
                implicitHeight: errorRow.implicitHeight + 18

                RowLayout {
                    id: errorRow
                    anchors.fill: parent
                    anchors.margins: 9
                    spacing: 10

                    Label {
                        Layout.fillWidth: true
                        text: appViewModel.errorMessage
                        color: theme.errorText
                        wrapMode: Text.WordWrap
                        font.pixelSize: 13
                    }

                    ModernButton {
                        text: t("action.dismiss")
                        onClicked: appViewModel.clearError()
                    }
                }
            }

            StackLayout {
                id: pageStack
                Layout.fillWidth: true
                Layout.fillHeight: true
                property int previousIndex: -1
                property bool transitionReady: false
                currentIndex: appViewModel.currentView === "services" ? 0
                    : appViewModel.currentView === "home" ? 1
                    : appViewModel.currentView === "library" ? 2
                    : appViewModel.currentView === "search" ? 3
                    : appViewModel.currentView === "details" ? 4
                    : appViewModel.currentView === "player" ? 5
                    : appViewModel.currentView === "iptv" ? 6
                    : appViewModel.currentView === "webdav" ? 7
                    : appViewModel.currentView === "transfers" ? 8
                    : appViewModel.currentView === "history" ? 9
                    : appViewModel.currentView === "scheduledTasks" ? 10
                    : 11

                transform: [
                    Translate {
                        id: pageTransitionOffset
                    },
                    Scale {
                        id: pageTransitionScale
                        origin.x: pageStack.width / 2
                        origin.y: pageStack.height / 2
                    }
                ]

                function resetTransition() {
                    pageEnterAnimation.stop()
                    pageStack.opacity = 1
                    pageTransitionOffset.x = 0
                    pageTransitionOffset.y = 0
                    pageTransitionScale.xScale = 1
                    pageTransitionScale.yScale = 1
                }

                function playTransition() {
                    var nextIndex = pageStack.currentIndex
                    var direction = pageStack.previousIndex < 0 || nextIndex >= pageStack.previousIndex ? 1 : -1
                    pageStack.previousIndex = nextIndex

                    if (!pageStack.transitionReady) {
                        return
                    }

                    if (!appViewModel.pageTransitionsEnabled) {
                        pageStack.resetTransition()
                        return
                    }

                    pageEnterAnimation.stop()
                    pageStack.opacity = 0.22
                    pageTransitionOffset.x = direction * 26
                    pageTransitionOffset.y = 6
                    pageTransitionScale.xScale = 0.992
                    pageTransitionScale.yScale = 0.992
                    pageEnterAnimation.start()
                }

                onCurrentIndexChanged: pageStack.playTransition()
                Component.onCompleted: {
                    pageStack.previousIndex = pageStack.currentIndex
                    pageStack.transitionReady = true
                    pageStack.resetTransition()
                }

                ParallelAnimation {
                    id: pageEnterAnimation

                    NumberAnimation {
                        target: pageStack
                        property: "opacity"
                        to: 1
                        duration: 210
                        easing.type: Easing.OutCubic
                    }

                    NumberAnimation {
                        target: pageTransitionOffset
                        properties: "x,y"
                        to: 0
                        duration: 280
                        easing.type: Easing.OutQuint
                    }

                    NumberAnimation {
                        target: pageTransitionScale
                        properties: "xScale,yScale"
                        to: 1
                        duration: 260
                        easing.type: Easing.OutCubic
                    }
                }

                Item {
                    GridView {
                        id: serviceGrid
                        anchors.fill: parent
                        clip: true
                        model: appViewModel.services
                        cellWidth: Math.max(270, width / Math.max(1, Math.floor(width / 300)))
                        cellHeight: 178
                        displaced: Transition {
                            NumberAnimation { properties: "x,y"; duration: 160; easing.type: Easing.OutCubic }
                        }

                        delegate: ServiceCard {
                            width: serviceGrid.cellWidth - 16
                            height: 156
                            editing: appViewModel.editingServices
                            serviceName: model.name
                            serviceType: model.serviceType
                            username: model.username
                            host: model.host.length > 0 ? model.host : model.baseUrl
                            autoLogin: model.autoLogin
                            hasSession: model.hasSession
                            privateMode: model.privateMode
                            dragIndex: index
                            onActivated: appViewModel.selectServiceCard(index)
                            onEditRequested: {
                                appViewModel.editServiceCard(index)
                                serviceDialog.open()
                            }
                            onDeleteRequested: {
                                root.pendingDeleteRow = index
                                deleteLocalDataCheck.checked = true
                                deleteDialog.open()
                            }
                            onDragStarted: root.dragFromRow = index
                            onDroppedOn: function(toRow) {
                                if (root.dragFromRow >= 0 && root.dragFromRow !== toRow) {
                                    appViewModel.moveServiceCardTo(root.dragFromRow, toRow)
                                }
                                root.dragFromRow = -1
                            }
                            onDragEnded: root.dragFromRow = -1
                        }
                    }

                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: 12
                        visible: serviceGrid.count === 0

                        Label {
                            text: appViewModel.privacyMode ? t("privacy.noCards") : t("empty.noServices")
                            color: theme.text
                            font.pixelSize: 24
                            font.bold: true
                        }

                        ModernButton {
                            text: appViewModel.privacyMode ? t("privacy.editCards") : t("empty.addService")
                            onClicked: {
                                if (appViewModel.privacyMode) {
                                    appViewModel.refreshPrivacyCards()
                                    privacyCardsDialog.open()
                                } else {
                                    appViewModel.beginAddServiceCard()
                                    serviceDialog.open()
                                }
                            }
                        }
                    }
                }

                Item {
                    id: homePage
                    property bool showInitialLoading: appViewModel.homeLoading
                        && appViewModel.continueItems.count === 0
                        && appViewModel.libraries.count === 0

                    Flickable {
                        id: homeFlick
                        anchors.fill: parent
                        contentWidth: width
                        contentHeight: homeColumn.implicitHeight
                        clip: true
                        opacity: homePage.showInitialLoading ? 0.24 : 1

                        Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }

                        ColumnLayout {
                            id: homeColumn
                            width: homeFlick.width
                            spacing: 28

                            SectionHeader {
                                title: t("section.continueWatching")
                                subtitle: t("section.continueSubtitle")
                            }

                            Item {
                                id: continueRail
                                Layout.fillWidth: true
                                Layout.preferredHeight: appViewModel.continueItems.count > 0 ? 316 : 72

                                function maxContentX() {
                                    return Math.max(0, continueList.contentWidth - continueList.width)
                                }

                                function scrollBy(delta) {
                                    continueList.contentX = Math.max(0, Math.min(maxContentX(), continueList.contentX + delta))
                                }

                                RowLayout {
                                    anchors.fill: parent
                                    visible: appViewModel.continueItems.count > 0
                                    spacing: 10

                                    IconButton {
                                        text: "‹"
                                        enabled: continueList.contentX > 1
                                        onClicked: continueRail.scrollBy(-Math.max(360, continueList.width * 0.82))
                                    }

                                    ListView {
                                        id: continueList
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        clip: true
                                        orientation: ListView.Horizontal
                                        boundsBehavior: Flickable.StopAtBounds
                                        spacing: 14
                                        model: appViewModel.continueItems

                                        delegate: ContinueWatchingCard {
                                            width: 172
                                            height: 306
                                            title: model.name.length > 0 ? model.name : model.seriesName
                                            seasonEpisode: appViewModel.formatSeasonEpisode(model.parentIndexNumber, model.indexNumber)
                                            progressText: appViewModel.formatContinueProgress(model.playedPercentage)
                                            imageUrl: model.continueImageUrl
                                            progress: model.playedPercentage
                                            onActivated: appViewModel.openContinueItem(index)
                                        }

                                        WheelHandler {
                                            onWheel: function(event) {
                                                var delta = event.angleDelta.y !== 0 ? -event.angleDelta.y : -event.angleDelta.x
                                                if (delta !== 0) {
                                                    continueRail.scrollBy(delta)
                                                    event.accepted = true
                                                }
                                            }
                                        }
                                    }

                                    IconButton {
                                        text: "›"
                                        enabled: continueList.contentX < continueRail.maxContentX() - 1
                                        onClicked: continueRail.scrollBy(Math.max(360, continueList.width * 0.82))
                                    }
                                }

                                MutedText {
                                    anchors.left: parent.left
                                    anchors.top: parent.top
                                    visible: appViewModel.continueItems.count === 0 && !homePage.showInitialLoading
                                    text: t("section.noProgress")
                                }
                            }

                            SectionHeader {
                                title: t("section.libraries")
                                subtitle: t("section.librariesSubtitle")
                            }

                            GridView {
                                id: libraryGrid
                                Layout.fillWidth: true
                                Layout.preferredHeight: Math.max(260, Math.ceil(count / Math.max(1, Math.floor(width / 214))) * 230)
                                clip: true
                                interactive: false
                                model: appViewModel.libraries
                                cellWidth: Math.max(190, width / Math.max(1, Math.floor(width / 214)))
                                cellHeight: 230

                                delegate: LibraryCard {
                                    width: libraryGrid.cellWidth - 16
                                    height: 210
                                    name: model.name
                                    subtitle: model.collectionType.length > 0 ? model.collectionType : model.itemType
                                    imageUrl: model.imageUrl
                                    onActivated: appViewModel.openLibrary(index)
                                }
                            }
                        }
                    }

                    PageLoadingPanel {
                        anchors.centerIn: parent
                        visible: homePage.showInitialLoading
                        title: t("loading.home")
                        subtitle: t("loading.homeHint")
                    }
                }

                Item {
                    id: libraryPage
                    property bool showInitialLoading: appViewModel.libraryItemsLoading && appViewModel.items.count === 0

                    GridView {
                        id: itemGrid
                        anchors.fill: parent
                        clip: true
                        model: appViewModel.items
                        cellWidth: Math.max(172, width / Math.max(1, Math.floor(width / 186)))
                        cellHeight: 292
                        opacity: libraryPage.showInitialLoading ? 0.24 : 1

                        Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }

                        onMovementEnded: {
                            if (atYEnd && appViewModel.loggedIn && !appViewModel.loading) {
                                appViewModel.loadMoreItems()
                            }
                        }

                        delegate: MediaPoster {
                            width: itemGrid.cellWidth - 14
                            height: 278
                            title: model.name
                            subtitle: model.productionYear.length > 0 ? model.productionYear + " · " + model.itemType : model.itemType
                            imageUrl: model.imageUrl
                            progress: model.playedPercentage
                            onActivated: appViewModel.openItem(index)
                        }
                    }

                    PageLoadingPanel {
                        anchors.centerIn: parent
                        visible: libraryPage.showInitialLoading
                        title: t("loading.library")
                        subtitle: t("loading.libraryHint")
                    }
                }

                Item {
                    id: serverSearchPage
                    property bool showInitialLoading: appViewModel.serverSearchLoading
                        && appViewModel.serverSearchResults.count === 0

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 14

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2

                                Label {
                                    Layout.fillWidth: true
                                    text: t("search.results")
                                    color: theme.text
                                    font.pixelSize: 21
                                    font.bold: true
                                    elide: Text.ElideRight
                                }

                                MutedText {
                                    Layout.fillWidth: true
                                    text: t("search.resultsFor") + " “" + appViewModel.activeServerSearchTerm + "”"
                                    elide: Text.ElideRight
                                }
                            }

                            Rectangle {
                                Layout.preferredWidth: searchResultCount.implicitWidth + 20
                                Layout.preferredHeight: 28
                                radius: 8
                                color: root.withAlpha(theme.primary, darkTheme ? 0.18 : 0.10)
                                border.color: root.withAlpha(theme.primary, 0.42)

                                Label {
                                    id: searchResultCount
                                    anchors.centerIn: parent
                                    text: t("search.resultCount").arg(appViewModel.serverSearchResults.count)
                                    color: theme.primary
                                    font.pixelSize: 12
                                    font.bold: true
                                }
                            }
                        }

                        Item {
                            Layout.fillWidth: true
                            Layout.fillHeight: true

                            GridView {
                                id: serverSearchGrid
                                anchors.fill: parent
                                clip: true
                                model: appViewModel.serverSearchResults
                                cellWidth: Math.max(172, width / Math.max(1, Math.floor(width / 186)))
                                cellHeight: 292
                                opacity: serverSearchPage.showInitialLoading ? 0.24 : 1

                                Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }

                                onMovementEnded: {
                                    if (atYEnd && appViewModel.serverSearchHasMore && !appViewModel.serverSearchLoading) {
                                        appViewModel.loadMoreServerSearchResults()
                                    }
                                }

                                delegate: MediaPoster {
                                    width: serverSearchGrid.cellWidth - 14
                                    height: 278
                                    title: model.name
                                    subtitle: model.itemType === "Episode" && model.seriesName.length > 0
                                        ? model.seriesName + (appViewModel.formatSeasonEpisode(model.parentIndexNumber, model.indexNumber).length > 0
                                            ? " · " + appViewModel.formatSeasonEpisode(model.parentIndexNumber, model.indexNumber) : "")
                                        : model.productionYear.length > 0 ? model.productionYear + " · " + model.itemType : model.itemType
                                    imageUrl: model.imageUrl
                                    progress: model.playedPercentage
                                    onActivated: appViewModel.openServerSearchItem(index)
                                }
                            }

                            ColumnLayout {
                                anchors.centerIn: parent
                                visible: !appViewModel.serverSearchLoading
                                    && appViewModel.serverSearchResults.count === 0
                                spacing: 9

                                Rectangle {
                                    Layout.alignment: Qt.AlignHCenter
                                    Layout.preferredWidth: 54
                                    Layout.preferredHeight: 54
                                    radius: 8
                                    color: root.withAlpha(theme.primary, darkTheme ? 0.20 : 0.12)
                                    border.color: root.withAlpha(theme.primary, 0.42)

                                    Label {
                                        anchors.centerIn: parent
                                        text: "\uD83D\uDD0D"
                                        font.pixelSize: 25
                                    }
                                }

                                Label {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: t("search.noResults")
                                    color: theme.text
                                    font.pixelSize: 18
                                    font.bold: true
                                }

                                MutedText {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: t("search.noResultsHint")
                                }
                            }

                            PageLoadingPanel {
                                anchors.centerIn: parent
                                visible: serverSearchPage.showInitialLoading
                                title: t("search.loading")
                                subtitle: t("search.loadingHint")
                            }
                        }

                        RowLayout {
                            visible: appViewModel.serverSearchLoading
                                && appViewModel.serverSearchResults.count > 0
                            Layout.alignment: Qt.AlignHCenter
                            Layout.preferredHeight: visible ? 30 : 0
                            spacing: 8

                            BusyIndicator {
                                running: parent.visible
                                implicitWidth: 24
                                implicitHeight: 24
                            }

                            MutedText {
                                text: t("search.loading")
                            }
                        }
                    }
                }

                DetailPage {}

                PlayerPage {}

                IptvPage {}

                WebDavPage {}

                TransfersPage {}

                HistoryPage {}

                ScheduledTasksPage {}

                SettingsPage {}
            }
        }
    }

    component ModernDialog: Dialog {
        id: modernDialog
        modal: true
        anchors.centerIn: parent
        padding: 18
        background: Rectangle {
            color: theme.surface
            radius: 10
            border.color: theme.border
        }
        header: Rectangle {
            visible: modernDialog.title.length > 0
            implicitHeight: modernDialog.title.length > 0 ? 54 : 0
            color: theme.surface
            radius: 10

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: theme.border
            }

            Label {
                anchors.fill: parent
                anchors.leftMargin: 18
                anchors.rightMargin: 18
                text: modernDialog.title
                color: theme.text
                font.pixelSize: 17
                font.bold: true
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }
        }
        footer: DialogButtonBox {
            visible: modernDialog.standardButtons !== Dialog.NoButton
            standardButtons: modernDialog.standardButtons
            alignment: Qt.AlignRight
            spacing: 10
            padding: 14
            onAccepted: modernDialog.accept()
            onRejected: modernDialog.reject()
            background: Rectangle {
                color: theme.surface
                radius: 10

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: 1
                    color: theme.border
                }
            }
            delegate: Button {
                id: dialogButton
                implicitHeight: 36
                leftPadding: 14
                rightPadding: 14
                font.pixelSize: 13
                font.bold: true
                contentItem: Label {
                    text: dialogButton.text
                    color: dialogButton.enabled ? (dialogButton.down ? "#ffffff" : theme.text) : theme.subtle
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    font: dialogButton.font
                }
                background: Rectangle {
                    radius: 8
                    color: dialogButton.down ? theme.primary : dialogButton.hovered ? theme.elevatedHover : theme.elevated
                    border.color: dialogButton.hovered ? theme.primary : theme.border
                }
            }
        }
        contentItem: ColumnLayout {
            spacing: 14
        }
    }

    component ModernButton: Button {
        id: modernButton
        property bool danger: false
        implicitHeight: 36
        leftPadding: 14
        rightPadding: 14
        font.pixelSize: 13
        font.bold: true
        contentItem: Label {
            text: modernButton.text
            color: modernButton.enabled ? (modernButton.down || modernButton.danger ? "#ffffff" : theme.text) : theme.subtle
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            font: modernButton.font
        }
        background: Rectangle {
            radius: 8
            color: modernButton.danger ? (modernButton.hovered ? theme.danger : theme.danger)
                : modernButton.down ? theme.primary
                : modernButton.hovered ? theme.elevatedHover
                : theme.elevated
            border.color: modernButton.danger ? theme.danger : modernButton.hovered ? theme.primary : theme.border
        }
    }

    component IconButton: ModernButton {
        implicitWidth: 38
        leftPadding: 0
        rightPadding: 0
    }

    component TransferFilterButton: Button {
        id: filterButton
        property bool selected: false
        implicitHeight: 34
        leftPadding: 10
        rightPadding: 10
        font.pixelSize: 13
        font.bold: selected
        contentItem: Label {
            text: filterButton.text
            color: filterButton.selected ? "#ffffff" : theme.muted
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            font: filterButton.font
        }
        background: Rectangle {
            radius: 5
            color: filterButton.selected
                ? theme.primary
                : filterButton.hovered ? theme.elevatedHover : "transparent"
            border.color: filterButton.selected
                ? theme.primary
                : filterButton.hovered ? theme.border : "transparent"
        }
    }

    component ModernTextField: TextField {
        id: field
        implicitHeight: 38
        color: theme.text
        placeholderTextColor: theme.subtle
        selectedTextColor: "#ffffff"
        selectionColor: theme.primary
        font.pixelSize: 14
        background: Rectangle {
            radius: 8
            color: theme.input
            border.color: field.activeFocus ? theme.primary : theme.border
        }
    }

    component MediaServerSearchBar: RowLayout {
        id: mediaServerSearchBar
        spacing: 8

        ModernTextField {
            id: serverSearchInput
            Layout.fillWidth: true
            implicitHeight: 38
            leftPadding: 36
            rightPadding: serverSearchClear.visible ? 38 : 12
            placeholderText: t("search.serverPlaceholder")
            text: appViewModel.serverSearchText
            onTextChanged: {
                if (appViewModel.serverSearchText !== text) {
                    appViewModel.serverSearchText = text
                }
            }
            onAccepted: {
                if (text.trim().length > 0) {
                    appViewModel.searchMediaServer()
                }
            }

            Label {
                anchors.left: parent.left
                anchors.leftMargin: 11
                anchors.verticalCenter: parent.verticalCenter
                text: "\uD83D\uDD0D"
                color: serverSearchInput.activeFocus ? theme.primary : theme.muted
                font.pixelSize: 15
                z: 2
            }

            Button {
                id: serverSearchClear
                anchors.right: parent.right
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                width: 30
                height: 30
                visible: serverSearchInput.text.length > 0
                text: "×"
                hoverEnabled: true
                z: 2
                ToolTip.visible: hovered
                ToolTip.text: t("search.clear")
                onClicked: appViewModel.clearServerSearch()

                contentItem: Label {
                    text: serverSearchClear.text
                    color: serverSearchClear.hovered ? theme.text : theme.muted
                    font.pixelSize: 17
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                background: Rectangle {
                    radius: 6
                    color: serverSearchClear.hovered ? theme.elevatedHover : "transparent"
                }
            }
        }

        ModernButton {
            Layout.preferredWidth: 82
            implicitHeight: 38
            text: t("search.action")
            enabled: serverSearchInput.text.trim().length > 0
            onClicked: appViewModel.searchMediaServer()
        }
    }

    component PinEntryField: Rectangle {
        id: pinEntry
        property alias text: pinInput.text
        property alias placeholderText: pinInput.placeholderText
        signal accepted()

        function forceActiveFocus() {
            pinInput.forceActiveFocus()
        }

        implicitHeight: 52
        radius: 10
        color: theme.input
        border.color: pinInput.activeFocus ? theme.primary : theme.border
        border.width: pinInput.activeFocus ? 2 : 1

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 12
            spacing: 10

            Rectangle {
                Layout.preferredWidth: 36
                Layout.preferredHeight: 36
                radius: 8
                color: pinInput.activeFocus ? theme.primary : theme.elevated
                border.color: pinInput.activeFocus ? theme.primary : theme.border

                Label {
                    anchors.centerIn: parent
                    text: "\uD83D\uDD12"
                    color: pinInput.activeFocus ? "#ffffff" : theme.muted
                    font.pixelSize: 17
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }

            TextField {
                id: pinInput
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: theme.text
                placeholderTextColor: theme.subtle
                selectedTextColor: "#ffffff"
                selectionColor: theme.primary
                echoMode: TextInput.Password
                inputMethodHints: Qt.ImhDigitsOnly
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                font.pixelSize: 20
                maximumLength: 12
                validator: RegularExpressionValidator { regularExpression: /^[0-9]*$/ }
                background: Item {}
                onAccepted: pinEntry.accepted()
            }
        }
    }

    component ModernComboBox: ComboBox {
        id: combo
        implicitHeight: 38
        font.pixelSize: 14
        contentItem: Label {
            text: combo.displayText
            color: theme.text
            verticalAlignment: Text.AlignVCenter
            leftPadding: 12
            rightPadding: 36
            elide: Text.ElideRight
        }
        indicator: Label {
            x: combo.width - width - 12
            y: combo.topPadding + (combo.availableHeight - height) / 2
            width: 18
            height: 18
            text: combo.popup.visible ? "^" : "v"
            color: combo.enabled ? theme.muted : theme.subtle
            font.pixelSize: 15
            font.bold: true
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: 8
            color: theme.input
            border.color: combo.activeFocus || combo.popup.visible ? theme.primary : theme.border
        }
        delegate: ItemDelegate {
            id: comboItem
            width: combo.popup.width - 12
            height: 38
            leftPadding: 12
            rightPadding: 12
            required property int index
            property bool current: combo.currentIndex === index

            contentItem: RowLayout {
                spacing: 10

                Label {
                    Layout.fillWidth: true
                    text: combo.textAt(comboItem.index)
                    color: comboItem.current ? "#ffffff" : theme.text
                    font.pixelSize: 14
                    font.bold: comboItem.current
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                }

                Rectangle {
                    visible: comboItem.current
                    Layout.preferredWidth: 8
                    Layout.preferredHeight: 8
                    radius: 4
                    color: "#ffffff"
                }
            }

            background: Rectangle {
                radius: 8
                color: comboItem.down ? theme.primary
                    : comboItem.current ? theme.primary
                    : comboItem.hovered ? theme.elevatedHover
                    : "transparent"
                border.color: comboItem.hovered && !comboItem.current ? theme.border : "transparent"
            }
        }
        popup: Popup {
            y: combo.height + 6
            width: combo.width
            implicitHeight: Math.min(contentItem.implicitHeight + 12, 252)
            padding: 6

            contentItem: ListView {
                clip: true
                implicitHeight: contentHeight
                model: combo.delegateModel
                currentIndex: combo.highlightedIndex
                spacing: 4
                boundsBehavior: Flickable.StopAtBounds
                ScrollIndicator.vertical: ScrollIndicator {}
            }

            background: Rectangle {
                radius: 10
                color: theme.elevated
                border.color: combo.popup.visible ? theme.primary : theme.border
            }
        }
    }

    component ModernCheckBox: CheckBox {
        id: check
        spacing: 8
        font.pixelSize: 14
        contentItem: Label {
            text: check.text
            color: theme.text
            verticalAlignment: Text.AlignVCenter
            leftPadding: check.indicator.width + check.spacing
            font: check.font
        }
        indicator: Rectangle {
            implicitWidth: 18
            implicitHeight: 18
            radius: 5
            x: 0
            y: parent.height / 2 - height / 2
            color: check.checked ? theme.primary : theme.input
            border.color: check.checked ? theme.primary : theme.border
            Label {
                anchors.centerIn: parent
                text: "✓"
                visible: check.checked
                color: "#ffffff"
                font.pixelSize: 12
                font.bold: true
            }
        }
    }

    component BodyText: Label {
        color: theme.text
        font.pixelSize: 14
    }

    component MutedText: Label {
        color: theme.muted
        font.pixelSize: 13
    }

    component SectionHeader: ColumnLayout {
        property string title: ""
        property string subtitle: ""
        spacing: 3

        Label {
            Layout.fillWidth: true
            text: title
            color: theme.text
            font.pixelSize: 22
            font.bold: true
            elide: Text.ElideRight
        }

        MutedText {
            Layout.fillWidth: true
            text: subtitle
            elide: Text.ElideRight
        }
    }

    component ThumbnailLoadingIcon: Item {
        id: loadingIcon
        property bool running: false
        property int iconSize: 26
        property color accentColor: theme.primary

        width: iconSize
        height: iconSize
        visible: running
        opacity: running ? 1 : 0

        Behavior on opacity { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }

        Rectangle {
            anchors.fill: parent
            radius: width / 2
            color: darkTheme ? "#b30f1217" : "#d9ffffff"
            border.color: darkTheme ? "#4dffffff" : "#99d8e0ea"
        }

        Item {
            id: spinnerDots
            anchors.centerIn: parent
            width: Math.max(12, loadingIcon.width - 10)
            height: width

            RotationAnimation on rotation {
                running: loadingIcon.running
                from: 0
                to: 360
                duration: 900
                loops: Animation.Infinite
                easing.type: Easing.Linear
            }

            Repeater {
                model: 8

                Rectangle {
                    property real angle: (index * 45 - 90) * Math.PI / 180
                    width: Math.max(2, Math.round(spinnerDots.width * 0.14))
                    height: width
                    radius: width / 2
                    x: spinnerDots.width / 2 - width / 2 + Math.cos(angle) * spinnerDots.width * 0.38
                    y: spinnerDots.height / 2 - height / 2 + Math.sin(angle) * spinnerDots.height * 0.38
                    color: loadingIcon.accentColor
                    opacity: 0.25 + index * 0.08
                }
            }
        }
    }

    component PageLoadingPanel: Rectangle {
        id: loadingPanel
        property string title: ""
        property string subtitle: ""

        width: Math.min(360, Math.max(260, parent ? parent.width - 56 : 320))
        height: 136
        radius: 10
        color: darkTheme ? "#e6171c22" : "#f7ffffff"
        border.color: darkTheme ? "#4d6f7b89" : "#d8d8e0ea"
        opacity: visible ? 1 : 0

        Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }

        Column {
            anchors.centerIn: parent
            width: parent.width - 40
            spacing: 10

            BusyIndicator {
                anchors.horizontalCenter: parent.horizontalCenter
                running: loadingPanel.visible
                implicitWidth: 36
                implicitHeight: 36
            }

            Label {
                width: parent.width
                text: loadingPanel.title
                color: theme.text
                font.pixelSize: 17
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
            }

            MutedText {
                width: parent.width
                text: loadingPanel.subtitle
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
            }
        }
    }

    component PosterImage: Rectangle {
        property string imageUrl: ""
        property string fallbackText: "?"
        radius: 8
        color: theme.input
        border.color: theme.border
        clip: true

        Image {
            id: posterImage
            anchors.fill: parent
            source: imageUrl
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
        }

        ThumbnailLoadingIcon {
            anchors.centerIn: parent
            iconSize: Math.min(30, Math.max(20, Math.round(Math.min(parent.width, parent.height) * 0.18)))
            running: imageUrl.length > 0 && posterImage.status === Image.Loading
        }

        Label {
            anchors.centerIn: parent
            visible: imageUrl.length === 0 || posterImage.status === Image.Error
            text: fallbackText
            color: theme.subtle
            font.pixelSize: 38
            font.bold: true
        }
    }

    component ServiceTypeIcon: Item {
        id: serviceIcon
        property string serviceType: ""
        readonly property string normalizedType: serviceType.toLowerCase()
        readonly property color accentColor: root.serviceAccentColor(serviceType)

        implicitWidth: 54
        implicitHeight: 54

        Rectangle {
            x: 5
            y: 7
            width: parent.width - 10
            height: parent.height - 9
            radius: 14
            color: root.withAlpha(serviceIcon.accentColor, darkTheme ? 0.30 : 0.18)
        }

        Rectangle {
            id: serviceIconPlate
            anchors.fill: parent
            anchors.margins: 3
            radius: 14
            color: serviceIcon.accentColor
            clip: true

            Rectangle {
                anchors.fill: parent
                radius: parent.radius
                color: "transparent"
                gradient: Gradient {
                    orientation: Gradient.Vertical
                    GradientStop { position: 0.0; color: "#38ffffff" }
                    GradientStop { position: 0.55; color: "#08ffffff" }
                    GradientStop { position: 1.0; color: "#18000000" }
                }
            }

            Canvas {
                id: serviceIconCanvas
                anchors.centerIn: parent
                width: 34
                height: 34
                antialiasing: true

                function roundedRectPath(context, x, y, width, height, radius) {
                    context.beginPath()
                    context.moveTo(x + radius, y)
                    context.lineTo(x + width - radius, y)
                    context.quadraticCurveTo(x + width, y, x + width, y + radius)
                    context.lineTo(x + width, y + height - radius)
                    context.quadraticCurveTo(x + width, y + height, x + width - radius, y + height)
                    context.lineTo(x + radius, y + height)
                    context.quadraticCurveTo(x, y + height, x, y + height - radius)
                    context.lineTo(x, y + radius)
                    context.quadraticCurveTo(x, y, x + radius, y)
                    context.closePath()
                }

                onPaint: {
                    var context = getContext("2d")
                    context.clearRect(0, 0, width, height)
                    context.lineCap = "round"
                    context.lineJoin = "round"

                    if (serviceIcon.normalizedType === "emby") {
                        context.save()
                        context.translate(17, 17)
                        context.rotate(Math.PI / 4)
                        context.strokeStyle = "#ffffff"
                        context.lineWidth = 2.4
                        context.strokeRect(-9.5, -9.5, 19, 19)
                        context.restore()

                        context.fillStyle = "#ffffff"
                        context.beginPath()
                        context.moveTo(14, 11)
                        context.lineTo(14, 23)
                        context.lineTo(23, 17)
                        context.closePath()
                        context.fill()
                    } else if (serviceIcon.normalizedType === "jellyfin") {
                        context.fillStyle = "#ffffff"
                        context.beginPath()
                        context.moveTo(17, 4)
                        context.lineTo(31, 29)
                        context.lineTo(3, 29)
                        context.closePath()
                        context.fill()

                        context.fillStyle = serviceIcon.accentColor
                        context.beginPath()
                        context.moveTo(17, 10)
                        context.lineTo(25.5, 25.5)
                        context.lineTo(8.5, 25.5)
                        context.closePath()
                        context.fill()

                        context.fillStyle = "#ffffff"
                        context.beginPath()
                        context.moveTo(17, 14)
                        context.lineTo(22.5, 24)
                        context.lineTo(11.5, 24)
                        context.closePath()
                        context.fill()
                    } else if (serviceIcon.normalizedType === "webdav") {
                        context.fillStyle = "#ffffff"
                        context.beginPath()
                        context.moveTo(8, 26)
                        context.bezierCurveTo(4.5, 26, 3, 23.5, 3, 20.5)
                        context.bezierCurveTo(3, 17, 5.5, 14.5, 9, 14.2)
                        context.bezierCurveTo(10.6, 9.5, 15.2, 7.8, 19.2, 10.4)
                        context.bezierCurveTo(21.2, 11.7, 22.3, 13.5, 22.6, 15.5)
                        context.bezierCurveTo(27.5, 14.7, 31, 17.5, 31, 21.5)
                        context.bezierCurveTo(31, 24.3, 28.8, 26, 25.5, 26)
                        context.closePath()
                        context.fill()

                        context.fillStyle = serviceIcon.accentColor
                        context.font = "bold 8px sans-serif"
                        context.textAlign = "center"
                        context.textBaseline = "middle"
                        context.fillText("DAV", 17, 21)
                    } else if (serviceIcon.normalizedType === "iptv") {
                        context.strokeStyle = "#ffffff"
                        context.lineWidth = 2.3
                        context.beginPath()
                        context.moveTo(17, 8)
                        context.lineTo(12, 3.5)
                        context.moveTo(17, 8)
                        context.lineTo(22, 3.5)
                        context.stroke()

                        roundedRectPath(context, 4, 8, 26, 21, 5)
                        context.stroke()

                        context.fillStyle = "#ffffff"
                        context.beginPath()
                        context.moveTo(14, 13)
                        context.lineTo(14, 24)
                        context.lineTo(22, 18.5)
                        context.closePath()
                        context.fill()
                    } else {
                        context.strokeStyle = "#ffffff"
                        context.fillStyle = "#ffffff"
                        context.lineWidth = 2.2
                        context.beginPath()
                        context.moveTo(10, 12)
                        context.lineTo(24, 8)
                        context.lineTo(26, 23)
                        context.lineTo(12, 26)
                        context.closePath()
                        context.stroke()
                        for (var index = 0; index < 4; ++index) {
                            var nodeX = index === 0 ? 10 : index === 1 ? 24 : index === 2 ? 26 : 12
                            var nodeY = index === 0 ? 12 : index === 1 ? 8 : index === 2 ? 23 : 26
                            context.beginPath()
                            context.arc(nodeX, nodeY, 3, 0, Math.PI * 2)
                            context.fill()
                        }
                    }
                }

                Component.onCompleted: requestPaint()
            }
        }

        onServiceTypeChanged: serviceIconCanvas.requestPaint()
    }

    component ServiceStatusChip: Rectangle {
        id: statusChip
        property string text: ""
        property color accentColor: theme.subtle

        implicitWidth: statusContent.implicitWidth + 18
        implicitHeight: 26
        radius: 9
        color: root.withAlpha(accentColor, darkTheme ? 0.14 : 0.09)
        border.color: root.withAlpha(accentColor, 0.38)

        RowLayout {
            id: statusContent
            anchors.fill: parent
            anchors.leftMargin: 9
            anchors.rightMargin: 9
            spacing: 6

            Rectangle {
                Layout.preferredWidth: 6
                Layout.preferredHeight: 6
                radius: 3
                color: statusChip.accentColor
            }

            Label {
                Layout.fillWidth: true
                text: statusChip.text
                color: statusChip.accentColor
                font.pixelSize: 11
                font.bold: true
                elide: Text.ElideRight
            }
        }
    }

    component ServiceCard: Rectangle {
        id: card
        signal activated()
        signal editRequested()
        signal deleteRequested()
        signal dragStarted()
        signal dragEnded()
        signal droppedOn(int toRow)
        property bool editing: false
        property string serviceName: ""
        property string serviceType: ""
        property string username: ""
        property string host: ""
        property bool autoLogin: true
        property bool hasSession: false
        property bool privateMode: false
        property int dragIndex: -1
        property real dragStartX: 0
        property real dragStartY: 0
        readonly property color accentColor: root.serviceAccentColor(serviceType)

        radius: 14
        color: cardMouse.containsMouse || dropArea.containsDrag ? theme.elevatedHover : theme.elevated
        border.color: dropArea.containsDrag ? theme.primary
            : cardMouse.containsMouse ? root.withAlpha(accentColor, 0.72)
            : theme.border
        border.width: dropArea.containsDrag ? 2 : 1
        scale: cardMouse.drag.active ? 0.98 : (cardMouse.containsMouse && !editing ? 1.008 : 1.0)
        opacity: cardMouse.drag.active ? 0.92 : 1.0
        z: cardMouse.drag.active ? 10 : 0
        Drag.active: cardMouse.drag.active && editing
        Drag.source: card
        Drag.hotSpot.x: width / 2
        Drag.hotSpot.y: height / 2

        Behavior on color { ColorAnimation { duration: 140; easing.type: Easing.OutCubic } }
        Behavior on border.color { ColorAnimation { duration: 140; easing.type: Easing.OutCubic } }
        Behavior on scale { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
        Behavior on opacity { NumberAnimation { duration: 120 } }

        function beginDrag() {
            dragStartX = x
            dragStartY = y
            dragStarted()
        }

        function finishDrag() {
            Drag.drop()
            dragEnded()
            x = dragStartX
            y = dragStartY
        }

        DropArea {
            id: dropArea
            anchors.fill: parent
            enabled: editing
            onDropped: card.droppedOn(card.dragIndex)
        }

        Rectangle {
            anchors.fill: parent
            anchors.margins: 1
            radius: card.radius - 1
            color: "transparent"
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop {
                    position: 0.0
                    color: root.withAlpha(card.accentColor,
                        cardMouse.containsMouse || dropArea.containsDrag
                            ? (darkTheme ? 0.15 : 0.10)
                            : (darkTheme ? 0.09 : 0.055))
                }
                GradientStop { position: 0.65; color: root.withAlpha(card.accentColor, 0.0) }
            }
        }

        Rectangle {
            anchors.left: parent.left
            anchors.leftMargin: 1
            anchors.verticalCenter: parent.verticalCenter
            width: 3
            height: parent.height - 34
            radius: 1.5
            color: card.accentColor
            opacity: cardMouse.containsMouse || dropArea.containsDrag ? 0.95 : 0.62

            Behavior on opacity { NumberAnimation { duration: 140 } }
        }

        MouseArea {
            id: cardMouse
            anchors.fill: parent
            hoverEnabled: true
            drag.target: editing ? card : null
            drag.axis: Drag.XAndYAxis
            onPressed: if (editing) card.beginDrag()
            onReleased: {
                if (editing) {
                    card.finishDrag()
                }
            }
            onCanceled: if (editing) card.finishDrag()
            onClicked: if (!editing) card.activated()
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 14
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                ServiceTypeIcon {
                    Layout.preferredWidth: 54
                    Layout.preferredHeight: 54
                    Layout.alignment: Qt.AlignTop
                    serviceType: card.serviceType
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 5

                        Label {
                            Layout.fillWidth: true
                            text: serviceName
                            color: theme.text
                            font.pixelSize: 17
                            font.bold: true
                            elide: Text.ElideRight
                        }

                        RowLayout {
                            visible: editing
                            spacing: 4

                            IconButton {
                                implicitWidth: 30
                                implicitHeight: 30
                                text: "✎"
                                onClicked: editRequested()
                            }

                            IconButton {
                                implicitWidth: 30
                                implicitHeight: 30
                                text: "×"
                                onClicked: deleteRequested()
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        Rectangle {
                            Layout.preferredWidth: serviceTypeLabel.implicitWidth + 16
                            Layout.preferredHeight: 21
                            radius: 7
                            color: root.withAlpha(card.accentColor, darkTheme ? 0.18 : 0.10)
                            border.color: root.withAlpha(card.accentColor, 0.40)

                            Label {
                                id: serviceTypeLabel
                                anchors.centerIn: parent
                                text: card.serviceType
                                color: card.accentColor
                                font.pixelSize: 10
                                font.bold: true
                            }
                        }

                        Rectangle {
                            visible: privateMode
                            Layout.preferredWidth: privateModeLabel.implicitWidth + 14
                            Layout.preferredHeight: 21
                            radius: 7
                            color: root.withAlpha(theme.primary, darkTheme ? 0.18 : 0.10)
                            border.color: root.withAlpha(theme.primary, 0.42)

                            Label {
                                id: privateModeLabel
                                anchors.centerIn: parent
                                text: t("history.privateBadge")
                                color: theme.primary
                                font.pixelSize: 10
                                font.bold: true
                            }
                        }

                        MutedText {
                            visible: username.length > 0
                            Layout.fillWidth: true
                            text: username
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                    }

                    MutedText {
                        Layout.fillWidth: true
                        text: host
                        color: theme.subtle
                        font.pixelSize: 11
                        elide: Text.ElideMiddle
                    }
                }
            }

            Item { Layout.fillHeight: true }

            RowLayout {
                id: serviceStatusRow
                Layout.fillWidth: true
                spacing: 8

                ServiceStatusChip {
                    Layout.maximumWidth: serviceStatusRow.width * 0.62
                    text: autoLogin ? t("status.autoLogin") : t("status.passwordRequired")
                    accentColor: autoLogin ? theme.success : theme.warning
                }

                Item { Layout.fillWidth: true }

                ServiceStatusChip {
                    Layout.maximumWidth: serviceStatusRow.width * 0.46
                    text: hasSession ? t("status.ready") : t("status.noSession")
                    accentColor: hasSession ? theme.primary : theme.subtle
                }
            }
        }
    }

    component LibraryCard: Rectangle {
        id: libraryCard
        signal activated()
        property string name: ""
        property string subtitle: ""
        property string imageUrl: ""

        radius: 10
        color: mouse.containsMouse ? theme.elevatedHover : theme.elevated
        border.color: theme.border

        MouseArea {
            id: mouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: activated()
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 10

            PosterImage {
                Layout.fillWidth: true
                Layout.preferredHeight: 126
                imageUrl: libraryCard.imageUrl
                fallbackText: libraryCard.name.length > 0 ? libraryCard.name[0] : "?"
            }

            Label {
                Layout.fillWidth: true
                text: libraryCard.name
                color: theme.text
                font.pixelSize: 15
                font.bold: true
                elide: Text.ElideRight
            }

            MutedText {
                Layout.fillWidth: true
                text: libraryCard.subtitle
                elide: Text.ElideRight
            }
        }
    }

    component MediaPoster: Rectangle {
        id: mediaPoster
        signal activated()
        property string title: ""
        property string subtitle: ""
        property string imageUrl: ""
        property real progress: 0

        radius: 10
        color: posterMouse.containsMouse ? theme.elevatedHover : theme.elevated
        border.color: theme.border
        clip: true
        scale: posterMouse.containsMouse ? 0.985 : 1.0

        Behavior on scale { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }

        MouseArea {
            id: posterMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: activated()
        }

        PosterImage {
            anchors.fill: parent
            imageUrl: mediaPoster.imageUrl
            fallbackText: mediaPoster.title.length > 0 ? mediaPoster.title[0] : "?"
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: Math.min(parent.height * 0.44, 118)
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#00000000" }
                GradientStop { position: 0.38; color: "#99000000" }
                GradientStop { position: 1.0; color: "#e6000000" }
            }
        }

        ColumnLayout {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            anchors.bottomMargin: 12
            spacing: 4

            Label {
                Layout.fillWidth: true
                text: mediaPoster.title
                color: "#ffffff"
                font.pixelSize: 14
                font.bold: true
                lineHeight: 0.92
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                elide: Text.ElideRight
                style: Text.Raised
                styleColor: "#cc000000"
            }

            Label {
                Layout.fillWidth: true
                text: mediaPoster.subtitle
                color: "#d8e1ee"
                font.pixelSize: 12
                elide: Text.ElideRight
                style: Text.Raised
                styleColor: "#bb000000"
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 5
                radius: 2
                color: "#66ffffff"
                visible: mediaPoster.progress > 0

                Rectangle {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    radius: 2
                    width: parent.width * Math.min(100, mediaPoster.progress) / 100
                    color: theme.primary
                }
            }
        }
    }

    component ContinueWatchingCard: Rectangle {
        id: continueCard
        signal activated()
        property string title: ""
        property string seasonEpisode: ""
        property string progressText: ""
        property string imageUrl: ""
        property real progress: 0

        radius: 10
        color: cardMouse.containsMouse ? theme.elevatedHover : theme.elevated
        border.color: theme.border
        clip: true
        scale: cardMouse.containsMouse ? 0.985 : 1.0

        Behavior on scale { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }

        MouseArea {
            id: cardMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: activated()
        }

        PosterImage {
            anchors.fill: parent
            imageUrl: continueCard.imageUrl
            fallbackText: continueCard.title.length > 0 ? continueCard.title[0] : "?"
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: Math.min(parent.height * 0.50, 150)
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#00000000" }
                GradientStop { position: 0.34; color: "#a3000000" }
                GradientStop { position: 1.0; color: "#e8000000" }
            }
        }

        ColumnLayout {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            anchors.bottomMargin: 12
            spacing: 4

            Label {
                Layout.fillWidth: true
                text: continueCard.title
                color: "#ffffff"
                font.pixelSize: 14
                font.bold: true
                lineHeight: 0.92
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                elide: Text.ElideRight
                style: Text.Raised
                styleColor: "#cc000000"
            }

            Label {
                Layout.fillWidth: true
                visible: continueCard.seasonEpisode.length > 0
                text: continueCard.seasonEpisode
                color: "#d8e1ee"
                font.pixelSize: 12
                elide: Text.ElideRight
                style: Text.Raised
                styleColor: "#bb000000"
            }

            Label {
                Layout.fillWidth: true
                text: continueCard.progressText
                color: "#c0cada"
                font.pixelSize: 12
                elide: Text.ElideRight
                style: Text.Raised
                styleColor: "#bb000000"
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 5
                radius: 2
                color: "#66ffffff"

                Rectangle {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    radius: 2
                    width: parent.width * Math.min(100, continueCard.progress) / 100
                    color: theme.primary
                }
            }
        }
    }

    component PersonCard: Rectangle {
        id: personCard
        property string name: ""
        property string roleName: ""
        property string imageUrl: ""

        radius: 10
        color: theme.elevated
        border.color: theme.border
        clip: true

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 8
            spacing: 8

            PosterImage {
                Layout.fillWidth: true
                Layout.preferredHeight: 142
                imageUrl: personCard.imageUrl
                fallbackText: personCard.name.length > 0 ? personCard.name[0] : "?"
            }

            Label {
                Layout.fillWidth: true
                text: personCard.name
                color: theme.text
                font.pixelSize: 13
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
            }

            MutedText {
                Layout.fillWidth: true
                text: personCard.roleName
                visible: personCard.roleName.length > 0
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
            }
        }
    }

    component SeasonPill: Rectangle {
        id: seasonPill
        signal activated()
        property string title: ""
        property bool selected: false

        radius: 8
        color: selected ? theme.primary : seasonMouse.containsMouse ? theme.elevatedHover : theme.elevated
        border.color: selected ? theme.primary : theme.border

        MouseArea {
            id: seasonMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: activated()
        }

        Label {
            id: seasonLabel
            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 14
            text: seasonPill.title
            color: seasonPill.selected ? "#ffffff" : theme.text
            font.pixelSize: 13
            font.bold: true
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }

    component EpisodeCard: Rectangle {
        id: episodeCard
        signal activated()
        property string title: ""
        property string subtitle: ""
        property string runtime: ""
        property string overview: ""
        property string imageUrl: ""
        property real progress: 0

        radius: 10
        color: episodeMouse.containsMouse ? theme.elevatedHover : theme.elevated
        border.color: theme.border
        clip: true

        MouseArea {
            id: episodeMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: activated()
        }

        RowLayout {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 12

            PosterImage {
                Layout.preferredWidth: 112
                Layout.fillHeight: true
                imageUrl: episodeCard.imageUrl
                fallbackText: episodeCard.title.length > 0 ? episodeCard.title[0] : "?"

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 4
                    color: theme.border
                    visible: episodeCard.progress > 0

                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: parent.width * Math.min(100, episodeCard.progress) / 100
                        color: theme.primary
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 4

                Label {
                    Layout.fillWidth: true
                    text: episodeCard.title
                    color: theme.text
                    font.pixelSize: 14
                    font.bold: true
                    elide: Text.ElideRight
                }

                MutedText {
                    Layout.fillWidth: true
                    text: episodeCard.runtime.length > 0 ? episodeCard.subtitle + " 路 " + episodeCard.runtime : episodeCard.subtitle
                    elide: Text.ElideRight
                }

                MutedText {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    text: episodeCard.overview
                    color: theme.subtle
                    wrapMode: Text.WordWrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                }
            }
        }
    }

    component IptvChannelCard: Rectangle {
        id: channelCard
        signal activated()
        property string title: ""
        property string groupName: ""
        property string logoUrl: ""

        radius: 10
        color: channelMouse.containsMouse ? theme.elevatedHover : theme.elevated
        border.color: channelMouse.containsMouse ? theme.primary : theme.border
        clip: true
        scale: channelMouse.containsMouse ? 0.985 : 1.0

        Behavior on scale { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }

        MouseArea {
            id: channelMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: channelCard.activated()
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 10

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 96
                radius: 8
                color: theme.input
                border.color: theme.border
                clip: true

                Image {
                    id: channelLogoImage
                    anchors.fill: parent
                    anchors.margins: 12
                    source: channelCard.logoUrl
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    visible: channelCard.logoUrl.length > 0
                }

                ThumbnailLoadingIcon {
                    anchors.centerIn: parent
                    iconSize: 24
                    running: channelCard.logoUrl.length > 0 && channelLogoImage.status === Image.Loading
                }

                Label {
                    anchors.centerIn: parent
                    visible: channelCard.logoUrl.length === 0
                    text: channelCard.title.length > 0 ? channelCard.title[0] : "?"
                    color: theme.subtle
                    font.pixelSize: 34
                    font.bold: true
                }
            }

            Label {
                Layout.fillWidth: true
                text: channelCard.title
                color: theme.text
                font.pixelSize: 15
                font.bold: true
                elide: Text.ElideRight
            }

            MutedText {
                Layout.fillWidth: true
                text: channelCard.groupName
                elide: Text.ElideRight
            }
        }
    }

    component WebDavFileRow: Rectangle {
        id: fileRow
        signal activated()
        signal downloadRequested()
        property string title: ""
        property string subtitle: ""
        property bool directory: false
        property bool playable: false

        radius: 8
        color: fileMouse.containsMouse ? theme.elevatedHover : theme.elevated
        border.color: fileMouse.containsMouse ? theme.primary : theme.border
        implicitHeight: 62

        MouseArea {
            id: fileMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: fileRow.activated()
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 10
            spacing: 12

            Label {
                text: directory ? "DIR" : playable ? "VID" : "FILE"
                color: directory ? theme.primary : playable ? theme.success : theme.subtle
                font.pixelSize: 12
                font.bold: true
                Layout.preferredWidth: 42
                horizontalAlignment: Text.AlignHCenter
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Label {
                    Layout.fillWidth: true
                    text: fileRow.title
                    color: theme.text
                    font.pixelSize: 15
                    font.bold: true
                    elide: Text.ElideRight
                }
                MutedText {
                    Layout.fillWidth: true
                    text: fileRow.subtitle
                    elide: Text.ElideRight
                }
            }

            ModernButton {
                text: t("action.download")
                onClicked: fileRow.downloadRequested()
            }
        }
    }

    component TransferTaskRow: Rectangle {
        id: taskRow
        property string taskId: ""
        property string title: ""
        property string direction: ""
        property string status: ""
        property string detail: ""
        property string target: ""
        property real bytesDone: 0
        property real bytesTotal: -1
        property real bytesPerSecond: 0
        property real averageBytesPerSecond: 0
        property real bytesRemaining: -1
        property real progress: 0
        property int fileCount: 0
        property int completedFileCount: 0
        property bool isGroup: false
        property bool cancellable: false
        property bool canPause: false
        property bool canResume: false
        property bool retryable: false
        signal activated()

        function directionIcon() {
            if (direction === "upload") {
                return "\u2191"
            }
            if (direction === "mkdir") {
                return "+"
            }
            return "\u2193"
        }

        function statusText() {
            switch (status) {
            case "queued": return t("transfers.statusQueued")
            case "running":
                if (direction === "upload") {
                    return t("transfers.statusUploading")
                }
                if (direction === "mkdir") {
                    return t("transfers.statusCreatingFolder")
                }
                return t("transfers.statusRunning")
            case "paused": return t("transfers.statusPaused")
            case "done": return t("transfers.statusDone")
            case "failed": return t("transfers.statusFailed")
            case "canceled": return t("transfers.statusCanceled")
            default: return status
            }
        }

        function statusColor() {
            switch (status) {
            case "done": return theme.success
            case "failed": return theme.danger
            case "canceled": return theme.subtle
            case "paused": return theme.warning
            case "running": return theme.primary
            default: return theme.warning
            }
        }

        radius: 8
        color: taskRow.isGroup && groupHover.hovered ? theme.elevatedHover : theme.elevated
        border.color: taskRow.isGroup && groupHover.hovered ? theme.primary : theme.border
        implicitHeight: taskRow.isGroup ? 154 : 116
        height: implicitHeight

        Behavior on color { ColorAnimation { duration: 120 } }
        Behavior on border.color { ColorAnimation { duration: 120 } }

        HoverHandler {
            id: groupHover
            enabled: taskRow.isGroup
            cursorShape: Qt.PointingHandCursor
        }

        TapHandler {
            enabled: taskRow.isGroup
            onTapped: taskRow.activated()
        }

        RowLayout {
            anchors.fill: parent
            anchors.margins: 14
            spacing: 14

            Rectangle {
                Layout.preferredWidth: taskRow.isGroup ? 48 : 42
                Layout.preferredHeight: taskRow.isGroup ? 48 : 42
                Layout.alignment: Qt.AlignTop
                radius: 8
                color: root.withAlpha(taskRow.statusColor(), darkTheme ? 0.18 : 0.11)
                border.color: root.withAlpha(taskRow.statusColor(), 0.42)

                Label {
                    anchors.centerIn: parent
                    text: taskRow.directionIcon()
                    color: taskRow.statusColor()
                    font.pixelSize: 22
                    font.bold: true
                }

                Rectangle {
                    visible: taskRow.isGroup && taskRow.fileCount > 0
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.rightMargin: -5
                    anchors.bottomMargin: -5
                    width: Math.max(20, groupCountLabel.implicitWidth + 8)
                    height: 20
                    radius: 7
                    color: theme.primary

                    Label {
                        id: groupCountLabel
                        anchors.centerIn: parent
                        text: taskRow.fileCount
                        color: "#ffffff"
                        font.pixelSize: 10
                        font.bold: true
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 6

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Label {
                        Layout.fillWidth: true
                        text: taskRow.title
                        color: theme.text
                        font.pixelSize: 15
                        font.bold: true
                        elide: Text.ElideRight
                    }

                    Rectangle {
                        Layout.preferredWidth: statusLabel.implicitWidth + 16
                        Layout.preferredHeight: 24
                        radius: 7
                        color: root.withAlpha(taskRow.statusColor(), darkTheme ? 0.16 : 0.10)

                        Label {
                            id: statusLabel
                            anchors.centerIn: parent
                            text: taskRow.statusText()
                            color: taskRow.statusColor()
                            font.pixelSize: 12
                            font.bold: true
                        }
                    }
                }

                MutedText {
                    Layout.fillWidth: true
                    text: taskRow.status === "failed" ? taskRow.detail : taskRow.target
                    color: taskRow.status === "failed" ? theme.danger : theme.muted
                    elide: Text.ElideMiddle
                }

                ProgressBar {
                    Layout.fillWidth: true
                    Layout.preferredHeight: taskRow.direction === "mkdir" ? 0 : 4
                    visible: taskRow.direction !== "mkdir"
                    from: 0
                    to: 1
                    value: taskRow.progress
                }

                RowLayout {
                    Layout.fillWidth: true

                    MutedText {
                        Layout.fillWidth: true
                        text: taskRow.isGroup
                            ? taskRow.completedFileCount + " / " + taskRow.fileCount + " " + t("transfers.files")
                            : taskRow.bytesTotal > 0
                                ? root.formatBytes(taskRow.bytesDone) + " / " + root.formatBytes(taskRow.bytesTotal)
                                : taskRow.detail
                        elide: Text.ElideRight
                    }

                    MutedText {
                        visible: taskRow.isGroup
                        text: taskRow.bytesTotal >= 0
                            ? root.formatBytes(taskRow.bytesDone) + " / " + root.formatBytes(taskRow.bytesTotal)
                            : t("transfers.unknown")
                        elide: Text.ElideRight
                    }

                    MutedText {
                        visible: !taskRow.isGroup
                            && taskRow.status === "running"
                            && taskRow.bytesPerSecond > 0
                        text: "\u2193 " + root.formatBytes(taskRow.bytesPerSecond) + "/s"
                        color: theme.text
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: taskRow.isGroup
                    spacing: 10

                    Label {
                        Layout.fillWidth: true
                        text: t("transfers.speed") + "  "
                            + root.formatBytes(taskRow.bytesPerSecond) + "/s"
                        color: taskRow.bytesPerSecond > 0 ? theme.primary : theme.muted
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }

                    Rectangle {
                        Layout.preferredWidth: 1
                        Layout.preferredHeight: 14
                        color: theme.border
                    }

                    Label {
                        Layout.fillWidth: true
                        text: t("transfers.averageSpeed") + "  "
                            + root.formatBytes(taskRow.averageBytesPerSecond) + "/s"
                        color: taskRow.averageBytesPerSecond > 0 ? theme.success : theme.muted
                        font.pixelSize: 12
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                    }

                    Rectangle {
                        Layout.preferredWidth: 1
                        Layout.preferredHeight: 14
                        color: theme.border
                    }

                    Label {
                        Layout.fillWidth: true
                        text: t("transfers.remaining") + "  "
                            + (taskRow.bytesRemaining >= 0
                                ? root.formatBytes(taskRow.bytesRemaining)
                                : t("transfers.unknown"))
                        color: taskRow.bytesRemaining > 0 ? theme.warning : theme.muted
                        font.pixelSize: 12
                        horizontalAlignment: Text.AlignRight
                        elide: Text.ElideRight
                    }
                }
            }

            ColumnLayout {
                Layout.fillHeight: true
                spacing: 4

                RowLayout {
                    Layout.alignment: Qt.AlignTop | Qt.AlignRight
                    spacing: 4

                    IconButton {
                        visible: taskRow.retryable
                        text: "\u21bb"
                        onClicked: appViewModel.retryTransfer(taskRow.taskId)
                        ToolTip.visible: hovered
                        ToolTip.text: taskRow.isGroup
                            ? t("transfers.retryTask")
                            : taskRow.direction === "upload"
                                ? t("transfers.retryUpload")
                                : t("transfers.retryFile")
                    }

                    IconButton {
                        visible: taskRow.canPause || taskRow.canResume
                        text: taskRow.canResume ? "\u25b6" : "\u2016"
                        onClicked: {
                            if (taskRow.canResume) {
                                appViewModel.resumeTransfer(taskRow.taskId)
                            } else {
                                appViewModel.pauseTransfer(taskRow.taskId)
                            }
                        }
                        ToolTip.visible: hovered
                        ToolTip.text: taskRow.direction === "upload"
                            ? taskRow.canResume
                                ? t("transfers.resumeUpload")
                                : t("transfers.pauseUpload")
                            : taskRow.canResume
                                ? t("transfers.resume")
                                : t("transfers.pause")
                    }

                    IconButton {
                        visible: taskRow.cancellable
                            && taskRow.status !== "done"
                            && taskRow.status !== "canceled"
                        text: "\u00d7"
                        danger: taskRow.isGroup
                        onClicked: appViewModel.cancelTransfer(taskRow.taskId)
                        ToolTip.visible: hovered
                        ToolTip.text: taskRow.isGroup
                            ? t("transfers.cancelTask")
                            : taskRow.direction === "upload"
                                ? t("transfers.cancelUpload")
                                : t("transfers.cancelFile")
                    }
                }

                Item { Layout.fillHeight: true }

                Label {
                    visible: taskRow.isGroup
                    Layout.alignment: Qt.AlignHCenter | Qt.AlignBottom
                    text: "\u203a"
                    color: groupHover.hovered ? theme.primary : theme.muted
                    font.pixelSize: 28
                    font.bold: true
                    ToolTip.visible: groupHover.hovered
                    ToolTip.text: t("transfers.openDetails")
                }
            }
        }
    }

    component TransferSummaryBlock: ColumnLayout {
        property string label: ""
        property string value: ""
        property color valueColor: theme.text

        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.preferredWidth: 0
        Layout.horizontalStretchFactor: 2
        spacing: 4

        MutedText {
            Layout.fillWidth: true
            text: label
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
        }

        Label {
            Layout.fillWidth: true
            text: value
            color: valueColor
            font.pixelSize: 20
            font.bold: true
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
        }
    }

    component TransferRateSummaryBlock: ColumnLayout {
        property string label: ""
        property real downloadRate: 0
        property real uploadRate: 0

        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.preferredWidth: 0
        Layout.horizontalStretchFactor: 3
        Layout.alignment: Qt.AlignVCenter
        spacing: 2
        clip: true

        MutedText {
            Layout.fillWidth: true
            text: label
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
        }

        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 5

            Label {
                text: "\u2193 " + t("transfers.downloadRate")
                color: theme.muted
                font.pixelSize: 11
            }
            Label {
                text: root.formatBytes(downloadRate) + "/s"
                color: downloadRate > 0 ? theme.primary : theme.text
                font.pixelSize: 12
                font.bold: true
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 5

            Label {
                text: "\u2191 " + t("transfers.uploadRate")
                color: theme.muted
                font.pixelSize: 11
            }
            Label {
                text: root.formatBytes(uploadRate) + "/s"
                color: uploadRate > 0 ? theme.success : theme.text
                font.pixelSize: 12
                font.bold: true
            }
        }
    }

    component DetailPage: Item {
        id: detailPage
        readonly property string backgroundImageUrl: appViewModel.selectedItemBackdropUrl.length > 0
            ? appViewModel.selectedItemBackdropUrl
            : appViewModel.selectedItemImageUrl
        readonly property bool usingPosterAsBackground: appViewModel.selectedItemBackdropUrl.length === 0
            && appViewModel.selectedItemImageUrl.length > 0

        Rectangle {
            anchors.fill: parent
            color: theme.bg
        }

        Item {
            id: detailArtworkLayer
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: Math.min(720, Math.max(520, detailPage.height * 0.92))
            clip: true
            visible: detailPage.backgroundImageUrl.length > 0

            Image {
                id: detailBackgroundImage
                anchors.fill: parent
                anchors.margins: detailPage.usingPosterAsBackground ? -54 : -24
                source: detailPage.backgroundImageUrl
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                smooth: true
                mipmap: true
                opacity: status === Image.Ready ? 1 : 0
                scale: detailPage.usingPosterAsBackground ? 1.08 : 1.03
                transformOrigin: Item.Center
                transform: Translate {
                    y: -Math.min(28, Math.max(0, detailFlick.contentY) * 0.06)
                }

                Behavior on opacity {
                    NumberAnimation { duration: 280; easing.type: Easing.OutCubic }
                }
            }

            Rectangle {
                anchors.fill: parent
                color: root.withAlpha(theme.bg, darkTheme ? 0.34 : 0.54)
            }

            Rectangle {
                anchors.fill: parent
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop {
                        position: 0.0
                        color: root.withAlpha(theme.bg, darkTheme ? 0.64 : 0.78)
                    }
                    GradientStop {
                        position: 0.56
                        color: root.withAlpha(theme.bg, darkTheme ? 0.22 : 0.46)
                    }
                    GradientStop {
                        position: 1.0
                        color: root.withAlpha(theme.bg, darkTheme ? 0.50 : 0.66)
                    }
                }
            }

            Rectangle {
                anchors.fill: parent
                gradient: Gradient {
                    orientation: Gradient.Vertical
                    GradientStop {
                        position: 0.0
                        color: root.withAlpha(theme.bg, darkTheme ? 0.10 : 0.20)
                    }
                    GradientStop {
                        position: 0.48
                        color: root.withAlpha(theme.bg, darkTheme ? 0.42 : 0.60)
                    }
                    GradientStop { position: 1.0; color: theme.bg }
                }
            }
        }

        Flickable {
            id: detailFlick
            anchors.fill: parent
            contentWidth: width
            contentHeight: detailColumn.implicitHeight + 16
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            ColumnLayout {
                id: detailColumn
                width: detailFlick.width
                spacing: 18

                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 360

                    Rectangle {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        anchors.topMargin: 10
                        radius: 18
                        color: darkTheme ? "#52000000" : "#260f1826"
                    }

                    Rectangle {
                        id: detailHeroCard
                        anchors.fill: parent
                        anchors.bottomMargin: 8
                        radius: 16
                        color: root.withAlpha(theme.surface, darkTheme ? 0.72 : 0.88)
                        border.width: 1
                        border.color: root.withAlpha(theme.border, darkTheme ? 0.92 : 0.84)
                        clip: true

                        Rectangle {
                            anchors.fill: parent
                            color: "transparent"
                            gradient: Gradient {
                                orientation: Gradient.Horizontal
                                GradientStop {
                                    position: 0.0
                                    color: root.withAlpha(theme.surface, darkTheme ? 0.28 : 0.44)
                                }
                                GradientStop {
                                    position: 0.62
                                    color: root.withAlpha(theme.surface, 0.04)
                                }
                                GradientStop {
                                    position: 1.0
                                    color: root.withAlpha(theme.primary, darkTheme ? 0.08 : 0.05)
                                }
                            }
                        }

                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            height: 1
                            color: darkTheme ? "#24ffffff" : "#b8ffffff"
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 24
                            spacing: 24

                            Item {
                                Layout.preferredWidth: 166
                                Layout.preferredHeight: 270
                                Layout.alignment: Qt.AlignVCenter

                                Rectangle {
                                    x: 8
                                    y: 10
                                    width: 150
                                    height: 250
                                    radius: 12
                                    color: darkTheme ? "#76000000" : "#300f1826"
                                }

                                PosterImage {
                                    x: 0
                                    y: 0
                                    width: 150
                                    height: 250
                                    radius: 12
                                    border.width: 1
                                    border.color: darkTheme ? "#38ffffff" : "#d8ffffff"
                                    imageUrl: appViewModel.selectedItemImageUrl
                                    fallbackText: appViewModel.selectedItemName.length > 0
                                        ? appViewModel.selectedItemName[0]
                                        : "?"
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                spacing: 10

                                Label {
                                    Layout.fillWidth: true
                                    text: appViewModel.selectedItemName
                                    color: theme.text
                                    font.pixelSize: 32
                                    font.bold: true
                                    wrapMode: Text.WordWrap
                                    maximumLineCount: 2
                                    elide: Text.ElideRight
                                }

                                MutedText {
                                    Layout.fillWidth: true
                                    text: appViewModel.selectedItemMeta
                                    wrapMode: Text.WordWrap
                                }

                                MutedText {
                                    Layout.fillWidth: true
                                    visible: appViewModel.selectedItemSeasonEpisode.length > 0
                                    text: appViewModel.selectedItemSeasonEpisode
                                    wrapMode: Text.WordWrap
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 5
                                    radius: 2
                                    color: root.withAlpha(theme.border, 0.88)
                                    visible: appViewModel.selectedItemPlayedPercentage > 0

                                    Rectangle {
                                        anchors.left: parent.left
                                        anchors.top: parent.top
                                        anchors.bottom: parent.bottom
                                        radius: 2
                                        width: parent.width * Math.min(100, appViewModel.selectedItemPlayedPercentage) / 100
                                        color: theme.primary
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 10

                                    ModernButton {
                                        text: appViewModel.selectedItemPlayedPercentage > 0
                                            ? t("action.continue")
                                            : t("action.play")
                                        visible: !appViewModel.selectedItemIsSeries
                                        enabled: !appViewModel.loading
                                        onClicked: appViewModel.playSelectedItem()
                                    }

                                    ModernButton {
                                        text: t("details.showOverview")
                                        enabled: appViewModel.selectedItemOverview.length > 0
                                        onClicked: overviewDialog.open()
                                    }

                                    Item { Layout.fillWidth: true }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 82
                                    radius: 10
                                    color: root.withAlpha(theme.bg, darkTheme ? 0.64 : 0.78)
                                    border.color: root.withAlpha(theme.border, darkTheme ? 0.82 : 0.72)
                                    clip: true

                                    BodyText {
                                        anchors.fill: parent
                                        anchors.margins: 12
                                        text: appViewModel.selectedItemOverview.length > 0
                                            ? appViewModel.selectedItemOverview
                                            : t("details.noOverview")
                                        color: theme.text
                                        wrapMode: Text.WordWrap
                                        maximumLineCount: 3
                                        elide: Text.ElideRight
                                        lineHeight: 1.1
                                    }
                                }

                                Item { Layout.fillHeight: true }
                            }
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    visible: appViewModel.selectedItemHasSeriesEpisodes
                    spacing: 14

                    SectionHeader {
                        title: t("details.seasonsEpisodes")
                        subtitle: appViewModel.selectedSeasonName.length > 0
                            ? appViewModel.selectedSeasonName
                            : t("details.noSeasons")
                    }

                    ListView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: appViewModel.seriesSeasons.count > 0 ? 54 : 0
                        visible: appViewModel.seriesSeasons.count > 0
                        clip: true
                        orientation: ListView.Horizontal
                        boundsBehavior: Flickable.StopAtBounds
                        spacing: 10
                        model: appViewModel.seriesSeasons

                        delegate: SeasonPill {
                            width: Math.min(190, Math.max(104, model.name.length * 9 + 34))
                            height: 42
                            title: model.name
                            selected: model.itemId === appViewModel.selectedSeasonId
                            onActivated: appViewModel.selectSeason(index)
                        }
                    }

                    GridView {
                        id: episodeGrid
                        Layout.fillWidth: true
                        Layout.preferredHeight: appViewModel.seriesEpisodes.count > 0
                            ? Math.ceil(appViewModel.seriesEpisodes.count / Math.max(1, Math.floor(width / 292))) * 118
                            : 46
                        visible: appViewModel.seriesSeasons.count > 0
                        clip: true
                        interactive: false
                        model: appViewModel.seriesEpisodes
                        cellWidth: Math.max(260, width / Math.max(1, Math.floor(width / 292)))
                        cellHeight: 112

                        delegate: EpisodeCard {
                            width: episodeGrid.cellWidth - 12
                            height: 100
                            title: model.name
                            subtitle: appViewModel.formatSeasonEpisode(model.parentIndexNumber, model.indexNumber)
                            runtime: model.runTime
                            overview: model.overview
                            imageUrl: model.imageUrl
                            progress: model.playedPercentage
                            onActivated: appViewModel.openEpisode(index)
                        }
                    }

                    MutedText {
                        Layout.fillWidth: true
                        visible: appViewModel.seriesSeasons.count === 0 && !appViewModel.loading
                        text: t("details.noSeasons")
                    }
                }

                SectionHeader {
                    title: t("details.castCrew")
                    subtitle: appViewModel.selectedItemPeopleModel.count > 0 ? "" : t("details.noCast")
                }

                ListView {
                    Layout.fillWidth: true
                    Layout.preferredHeight: appViewModel.selectedItemPeopleModel.count > 0 ? 218 : 0
                    visible: appViewModel.selectedItemPeopleModel.count > 0
                    clip: true
                    orientation: ListView.Horizontal
                    boundsBehavior: Flickable.StopAtBounds
                    spacing: 14
                    model: appViewModel.selectedItemPeopleModel

                    delegate: PersonCard {
                        width: 128
                        height: 210
                        name: model.name
                        roleName: model.roleName
                        imageUrl: model.imageUrl
                    }
                }
            }
        }
    }

    component PlayerPage: Item {
        id: playerPage
        property bool controlsVisible: true
        property bool exitConfirmVisible: false
        property real exitPositionSeconds: 0
        property bool immersive: root.playerImmersive
        property bool fullscreen: immersive || root.visibility === Window.FullScreen
        property int topChromeHeight: 72
        property int bottomChromeHeight: 146
        property bool seekLoadingActive: false
        property bool rawPlaybackLoading: mpvVideo.loading || mpvVideo.buffering || mpvVideo.seeking || seekLoadingActive
        property bool playbackLoadingVisible: false
        property bool videoInfoVisible: false
        property bool trackMenuVisible: false
        property bool iptvChannelListVisible: false
        property string trackMenuMode: "subtitle"
        property Item trackMenuAnchorItem: null
        property real trackMenuAnchorGlobalX: -1
        property var playbackSpeedOptions: [0.5, 0.75, 1.0, 1.25, 1.5, 2.0]
        focus: true

        function formatTime(seconds) {
            if (!Number.isFinite(seconds) || seconds <= 0) {
                return "00:00"
            }
            var total = Math.floor(seconds)
            var hours = Math.floor(total / 3600)
            var minutes = Math.floor((total % 3600) / 60)
            var secs = total % 60
            var mm = minutes < 10 ? "0" + minutes : "" + minutes
            var ss = secs < 10 ? "0" + secs : "" + secs
            if (hours > 0) {
                return hours + ":" + mm + ":" + ss
            }
            return mm + ":" + ss
        }

        function revealControls() {
            controlsVisible = true
            Qt.callLater(raiseChromeWindows)
            if (!exitConfirmVisible && !trackMenuVisible && !iptvChannelListVisible) {
                controlsHideTimer.restart()
            }
        }

        function toggleFullscreen() {
            if (immersive) {
                root.exitPlayerFullscreen()
                controlsVisible = true
            } else {
                root.enterPlayerFullscreen()
                revealControls()
            }
        }

        function requestExitPlayback() {
            closeTrackMenu(false)
            closeIptvChannelList(false)
            revealControls()
            exitPositionSeconds = mpvVideo.position
            exitConfirmVisible = true
        }

        function confirmExitPlayback() {
            appViewModel.reportPlaybackStopped(exitPositionSeconds)
            mpvVideo.stop()
            root.exitPlayerFullscreen()
            exitConfirmVisible = false
            videoInfoVisible = false
            trackMenuVisible = false
            iptvChannelListVisible = false
            appViewModel.closePlayerToDetails()
        }

        function cancelExitPlayback() {
            exitConfirmVisible = false
            revealControls()
        }

        function beginSeekLoading() {
            seekLoadingActive = true
            playbackLoadingDelay.restart()
            seekLoadingTimeout.restart()
        }

        function finishSeekLoading() {
            if (!seekLoadingActive) {
                return
            }
            seekLoadingActive = false
            seekLoadingTimeout.stop()
            if (!mpvVideo.loading && !mpvVideo.buffering && !mpvVideo.seeking) {
                playbackLoadingDelay.stop()
                playbackLoadingVisible = false
            }
        }

        function handlePlayerKey(event) {
            if (event.key === Qt.Key_Escape && iptvChannelListVisible) {
                closeIptvChannelList()
                event.accepted = true
            } else if (event.key === Qt.Key_Escape && trackMenuVisible) {
                closeTrackMenu()
                event.accepted = true
            } else if (event.key === Qt.Key_Space) {
                mpvVideo.togglePause()
                appViewModel.reportPlaybackProgress(mpvVideo.position, mpvVideo.paused)
                playerPage.revealControls()
                event.accepted = true
            } else if (event.key === Qt.Key_Right) {
                mpvVideo.seekRelative(15)
                playerPage.beginSeekLoading()
                appViewModel.reportPlaybackProgress(mpvVideo.position + 15, mpvVideo.paused)
                playerPage.revealControls()
                event.accepted = true
            } else if (event.key === Qt.Key_Left) {
                mpvVideo.seekRelative(-15)
                playerPage.beginSeekLoading()
                appViewModel.reportPlaybackProgress(Math.max(0, mpvVideo.position - 15), mpvVideo.paused)
                playerPage.revealControls()
                event.accepted = true
            } else if (event.key === Qt.Key_F) {
                playerPage.toggleFullscreen()
                event.accepted = true
            } else if (event.key === Qt.Key_Escape) {
                if (playerPage.immersive || root.visibility === Window.FullScreen) {
                    root.exitPlayerFullscreen()
                    playerPage.revealControls()
                } else {
                    playerPage.requestExitPlayback()
                }
                event.accepted = true
            }
        }

        function raiseChromeWindows() {
            if (playerLoadingWindow.visible) {
                playerLoadingWindow.syncLoadingGeometry()
                playerLoadingWindow.raise()
            }
            if (playerTopChromeWindow.visible) {
                playerTopChromeWindow.syncChromeGeometry()
                playerTopChromeWindow.raise()
            }
            if (playerBottomChromeWindow.visible) {
                playerBottomChromeWindow.syncChromeGeometry()
                playerBottomChromeWindow.raise()
            }
            if (playerInfoWindow.visible) {
                playerInfoWindow.syncInfoGeometry()
                playerInfoWindow.raise()
            }
            if (playerTrackMenuWindow.visible) {
                playerTrackMenuWindow.syncTrackMenuGeometry()
                playerTrackMenuWindow.raise()
            }
            if (playerIptvChannelWindow.visible) {
                playerIptvChannelWindow.syncChannelListGeometry()
                playerIptvChannelWindow.raise()
            }
        }

        function playbackLoadingTitle() {
            if (mpvVideo.buffering || playerPage.seekLoadingActive) {
                var progress = mpvVideo.bufferingProgress > 0 && mpvVideo.bufferingProgress < 100
                    ? " " + mpvVideo.bufferingProgress + "%"
                    : ""
                return t("player.buffering") + progress
            }
            if (mpvVideo.seeking) {
                return t("player.seeking")
            }
            return t("player.loading")
        }

        function videoInfoValue(value) {
            return value && value.length > 0 ? value : "--"
        }

        function cacheDurationText(seconds) {
            if (!Number.isFinite(seconds) || seconds < 0) {
                return "--"
            }
            if (seconds < 60) {
                return Math.round(seconds) + "s"
            }
            var minutes = Math.floor(seconds / 60)
            var remaining = Math.round(seconds % 60)
            return minutes + "m " + remaining + "s"
        }

        function openTrackMenu(mode, anchorItem) {
            if (trackMenuVisible && trackMenuMode === mode) {
                closeTrackMenu()
                return
            }
            closeIptvChannelList(false)
            trackMenuMode = mode
            trackMenuAnchorItem = anchorItem
            if (anchorItem) {
                var anchorGlobal = anchorItem.mapToGlobal(anchorItem.width / 2, 0)
                trackMenuAnchorGlobalX = anchorGlobal.x
            } else {
                trackMenuAnchorGlobalX = -1
            }
            trackMenuVisible = true
            controlsVisible = true
            controlsHideTimer.stop()
            Qt.callLater(raiseChromeWindows)
        }

        function closeTrackMenu(restoreControls) {
            if (!trackMenuVisible) {
                return
            }
            trackMenuVisible = false
            trackMenuAnchorItem = null
            trackMenuAnchorGlobalX = -1
            if (restoreControls === false) {
                return
            }
            revealControls()
        }

        function openIptvChannelList() {
            if (!appViewModel.iptvPlaybackActive) {
                return
            }
            if (iptvChannelListVisible) {
                closeIptvChannelList()
                return
            }
            closeTrackMenu(false)
            videoInfoVisible = false
            iptvChannelListVisible = true
            controlsVisible = true
            controlsHideTimer.stop()
            Qt.callLater(raiseChromeWindows)
            Qt.callLater(positionCurrentIptvChannel)
        }

        function closeIptvChannelList(restoreControls) {
            if (!iptvChannelListVisible) {
                return
            }
            iptvChannelListVisible = false
            if (restoreControls === false) {
                return
            }
            revealControls()
        }

        function positionCurrentIptvChannel() {
            if (!playerIptvChannelList || !appViewModel.iptvChannels) {
                return
            }
            var currentIndex = appViewModel.iptvChannels.indexOfChannelId(appViewModel.currentIptvChannelId)
            if (currentIndex >= 0) {
                playerIptvChannelList.positionViewAtIndex(currentIndex, ListView.Center)
            }
        }

        function trackMenuTitle() {
            if (trackMenuMode === "subtitle") {
                return t("player.subtitles")
            }
            if (trackMenuMode === "speed") {
                return t("player.speed")
            }
            return t("player.audio")
        }

        function trackMenuHint() {
            if (trackMenuMode === "subtitle") {
                return mpvVideo.subtitleTracks.count + " " + t("player.tracks")
            }
            if (trackMenuMode === "speed") {
                return t("player.currentSpeed") + " " + speedLabel(mpvVideo.speed)
            }
            return mpvVideo.audioTracks.count + " " + t("player.tracks")
        }

        function trackMenuRowCount() {
            if (trackMenuMode === "subtitle") {
                return mpvVideo.subtitleTracks.count + 1
            }
            if (trackMenuMode === "speed") {
                return playbackSpeedOptions.length
            }
            return mpvVideo.audioTracks.count
        }

        function speedLabel(speed) {
            var text = Number(speed).toFixed(2)
            text = text.replace(/0+$/, "")
            text = text.replace(/\.$/, "")
            return text + "x"
        }

        function speedSelected(speed) {
            return Math.abs(mpvVideo.speed - speed) < 0.01
        }

        Component.onCompleted: {
            forceActiveFocus()
            revealControls()
        }

        Connections {
            target: appViewModel
            function onCurrentViewChanged() {
                if (appViewModel.currentView === "player") {
                    playerPage.forceActiveFocus()
                    playerPage.revealControls()
                    if (playerPage.rawPlaybackLoading) {
                        playbackLoadingDelay.restart()
                    }
                } else {
                    playerPage.playbackLoadingVisible = false
                    playerPage.seekLoadingActive = false
                    playerPage.videoInfoVisible = false
                    playerPage.trackMenuVisible = false
                    playerPage.iptvChannelListVisible = false
                    playbackLoadingDelay.stop()
                    seekLoadingTimeout.stop()
                }
            }
        }

        MpvVideoItem {
            id: mpvVideo
            anchors.fill: parent
            startPosition: appViewModel.currentPlaybackStartSeconds
            source: appViewModel.currentPlaybackUrl
            httpUsername: appViewModel.playbackHttpUsername
            httpPassword: appViewModel.playbackHttpPassword
            allowInsecureTls: appViewModel.playbackAllowInsecureTls
            onErrorOccurred: function(message) {
                appViewModel.reportPlaybackError(message)
                console.warn(message)
            }
            onNativeWindowUpdated: {
                Qt.callLater(playerPage.raiseChromeWindows)
            }
            onPlaybackNetworkBytes: function(bytesReceived) {
                appViewModel.recordPlaybackNetworkBytes(bytesReceived)
            }
            onPlaybackRestarted: playerPage.finishSeekLoading()
            onPlaybackStateChanged: {
                if (playerPage.rawPlaybackLoading && appViewModel.currentView === "player") {
                    if (!playerPage.playbackLoadingVisible && !playbackLoadingDelay.running) {
                        playbackLoadingDelay.restart()
                    }
                } else {
                    playbackLoadingDelay.stop()
                    playerPage.playbackLoadingVisible = false
                }
                if (duration > 0 || position > 0) {
                    appViewModel.reportPlaybackStarted()
                }
                appViewModel.reportPlaybackProgress(position, paused)
            }
            Component.onCompleted: {
                play()
            }
        }

        MouseArea {
            anchors.fill: mpvVideo
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onPositionChanged: playerPage.revealControls()
            onClicked: {
                playerPage.closeTrackMenu(false)
                playerPage.closeIptvChannelList(false)
                playerPage.revealControls()
            }
            onDoubleClicked: {
                playerPage.toggleFullscreen()
            }
        }

        Keys.onPressed: function(event) {
            playerPage.handlePlayerKey(event)
        }

        Timer {
            id: controlsHideTimer
            interval: 2800
            repeat: false
            onTriggered: if (!playerPage.exitConfirmVisible && !playerPage.trackMenuVisible && !playerPage.iptvChannelListVisible) playerPage.controlsVisible = false
        }

        Timer {
            id: playbackProgressTimer
            interval: 15000
            running: appViewModel.currentView === "player"
            repeat: true
            onTriggered: appViewModel.reportPlaybackProgress(mpvVideo.position, mpvVideo.paused)
        }

        Timer {
            id: playbackLoadingDelay
            interval: 300
            repeat: false
            onTriggered: {
                playerPage.playbackLoadingVisible = playerPage.rawPlaybackLoading && appViewModel.currentView === "player"
                if (playerPage.playbackLoadingVisible) {
                    Qt.callLater(playerPage.raiseChromeWindows)
                }
            }
        }

        Timer {
            id: seekLoadingTimeout
            interval: 15000
            repeat: false
            onTriggered: playerPage.finishSeekLoading()
        }

        Window {
            id: playerLoadingWindow
            color: "transparent"
            flags: Qt.FramelessWindowHint | Qt.Tool | Qt.WindowTransparentForInput
            transientParent: root
            visible: appViewModel.currentView === "player" && root.visible && playerPage.playbackLoadingVisible

            function syncLoadingGeometry() {
                if (playerPage.width <= 0 || playerPage.height <= 0) {
                    return
                }
                var origin = playerPage.mapToGlobal(0, 0)
                x = Math.round(origin.x)
                y = Math.round(origin.y)
                width = Math.max(1, Math.round(playerPage.width))
                height = Math.max(1, Math.round(playerPage.height))
            }

            onVisibleChanged: {
                syncLoadingGeometry()
                if (visible) {
                    raise()
                }
            }

            Component.onCompleted: syncLoadingGeometry()

            Connections {
                target: root
                function onXChanged() { playerLoadingWindow.syncLoadingGeometry() }
                function onYChanged() { playerLoadingWindow.syncLoadingGeometry() }
                function onWidthChanged() { playerLoadingWindow.syncLoadingGeometry() }
                function onHeightChanged() { playerLoadingWindow.syncLoadingGeometry() }
                function onVisibilityChanged() { playerLoadingWindow.syncLoadingGeometry() }
                function onPlayerImmersiveChanged() { playerLoadingWindow.syncLoadingGeometry() }
            }

            Connections {
                target: playerPage
                function onWidthChanged() { playerLoadingWindow.syncLoadingGeometry() }
                function onHeightChanged() { playerLoadingWindow.syncLoadingGeometry() }
            }

            Item {
                anchors.fill: parent

                Rectangle {
                    id: playbackLoadingCard
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 48, 320)
                    height: 148
                    radius: 10
                    color: "#d90a0d12"
                    border.color: "#4d6f7b89"

                    Column {
                        anchors.centerIn: parent
                        width: parent.width - 48
                        spacing: 10

                        BusyIndicator {
                            anchors.horizontalCenter: parent.horizontalCenter
                            running: playerLoadingWindow.visible
                            implicitWidth: 42
                            implicitHeight: 42
                        }

                        Label {
                            width: parent.width
                            text: playerPage.playbackLoadingTitle()
                            color: "#ffffff"
                            font.pixelSize: 19
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            elide: Text.ElideRight
                        }

                        Label {
                            width: parent.width
                            text: t("player.networkHint")
                            color: "#cbd5e1"
                            font.pixelSize: 13
                            horizontalAlignment: Text.AlignHCenter
                            elide: Text.ElideRight
                        }
                    }
                }
            }
        }

        Window {
            id: playerInfoWindow
            color: "transparent"
            flags: Qt.FramelessWindowHint | Qt.Tool
            transientParent: root
            visible: appViewModel.currentView === "player" && root.visible && playerPage.videoInfoVisible

            function syncInfoGeometry() {
                if (playerPage.width <= 0 || playerPage.height <= 0) {
                    return
                }
                var panelWidth = Math.min(380, Math.max(300, playerPage.width - 48))
                var panelHeight = 264
                var margin = 24
                var localX = Math.max(margin, playerPage.width - panelWidth - margin)
                var localY = Math.min(playerPage.height - panelHeight - margin, playerPage.topChromeHeight + 18)
                var origin = playerPage.mapToGlobal(localX, Math.max(margin, localY))
                x = Math.round(origin.x)
                y = Math.round(origin.y)
                width = Math.round(panelWidth)
                height = panelHeight
            }

            onVisibleChanged: {
                syncInfoGeometry()
                if (visible) {
                    raise()
                    playerInfoRoot.forceActiveFocus()
                }
            }

            Component.onCompleted: syncInfoGeometry()

            Connections {
                target: root
                function onXChanged() { playerInfoWindow.syncInfoGeometry() }
                function onYChanged() { playerInfoWindow.syncInfoGeometry() }
                function onWidthChanged() { playerInfoWindow.syncInfoGeometry() }
                function onHeightChanged() { playerInfoWindow.syncInfoGeometry() }
                function onVisibilityChanged() { playerInfoWindow.syncInfoGeometry() }
                function onPlayerImmersiveChanged() { playerInfoWindow.syncInfoGeometry() }
            }

            Connections {
                target: playerPage
                function onWidthChanged() { playerInfoWindow.syncInfoGeometry() }
                function onHeightChanged() { playerInfoWindow.syncInfoGeometry() }
            }

            Rectangle {
                id: playerInfoRoot
                anchors.fill: parent
                focus: true
                radius: 10
                color: "#e60b0f16"
                border.color: "#667c8796"

                Keys.onPressed: function(event) {
                    if (event.key === Qt.Key_Escape) {
                        playerPage.videoInfoVisible = false
                        playerPage.revealControls()
                        event.accepted = true
                    } else {
                        playerPage.handlePlayerKey(event)
                    }
                }

                HoverHandler {
                    onHoveredChanged: if (hovered) playerPage.revealControls()
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 18
                    spacing: 14

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        Label {
                            Layout.fillWidth: true
                            text: t("player.videoInfo")
                            color: "#ffffff"
                            font.pixelSize: 21
                            font.bold: true
                            elide: Text.ElideRight
                        }

                        Button {
                            id: closeInfoButton
                            text: "X"
                            implicitWidth: 34
                            implicitHeight: 34
                            leftPadding: 0
                            rightPadding: 0
                            contentItem: Label {
                                text: closeInfoButton.text
                                color: "#ffffff"
                                font.pixelSize: 18
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                radius: 8
                                color: closeInfoButton.down ? "#4f8cff" : closeInfoButton.hovered ? "#354253" : "#22313d"
                                border.color: closeInfoButton.hovered ? "#6aa0ff" : "#405061"
                            }
                            onClicked: {
                                playerPage.videoInfoVisible = false
                                playerPage.revealControls()
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: "#334b5563"
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: 18
                        rowSpacing: 11

                        MutedText {
                            text: t("player.resolution")
                            color: "#aeb8c6"
                        }
                        BodyText {
                            Layout.fillWidth: true
                            text: playerPage.videoInfoValue(mpvVideo.videoResolution)
                            color: "#ffffff"
                            font.bold: true
                            elide: Text.ElideRight
                        }

                        MutedText {
                            text: t("player.codec")
                            color: "#aeb8c6"
                        }
                        BodyText {
                            Layout.fillWidth: true
                            text: playerPage.videoInfoValue(mpvVideo.videoCodec)
                            color: "#ffffff"
                            font.bold: true
                            elide: Text.ElideRight
                        }

                        MutedText {
                            text: t("player.frameRate")
                            color: "#aeb8c6"
                        }
                        BodyText {
                            Layout.fillWidth: true
                            text: playerPage.videoInfoValue(mpvVideo.videoFrameRate)
                            color: "#ffffff"
                            font.bold: true
                            elide: Text.ElideRight
                        }

                        MutedText {
                            text: t("player.bitrate")
                            color: "#aeb8c6"
                        }
                        BodyText {
                            Layout.fillWidth: true
                            text: playerPage.videoInfoValue(mpvVideo.videoBitrate)
                            color: "#ffffff"
                            font.bold: true
                            elide: Text.ElideRight
                        }

                        MutedText {
                            text: t("player.cacheDuration")
                            color: "#aeb8c6"
                        }
                        BodyText {
                            Layout.fillWidth: true
                            text: playerPage.cacheDurationText(mpvVideo.cacheDurationSeconds)
                            color: "#ffffff"
                            font.bold: true
                            elide: Text.ElideRight
                        }
                    }

                    MutedText {
                        Layout.fillWidth: true
                        text: t("player.infoHint")
                        color: "#8794a5"
                        elide: Text.ElideRight
                    }
                }
            }
        }

        Window {
            id: playerTrackMenuWindow
            color: "transparent"
            flags: Qt.FramelessWindowHint | Qt.Tool
            transientParent: root
            visible: appViewModel.currentView === "player" && root.visible && playerPage.trackMenuVisible

            function syncTrackMenuGeometry() {
                if (playerPage.width <= 0 || playerPage.height <= 0) {
                    return
                }
                var panelWidth = Math.min(360, Math.max(280, playerPage.width - 48))
                var rowCount = Math.max(1, playerPage.trackMenuRowCount())
                var panelHeight = Math.min(360, 94 + rowCount * 48)
                var margin = 20
                var playerOrigin = playerPage.mapToGlobal(0, 0)
                var localX = 0
                if (playerPage.trackMenuAnchorItem) {
                    var anchorGlobal = playerPage.trackMenuAnchorItem.mapToGlobal(playerPage.trackMenuAnchorItem.width / 2, 0)
                    playerPage.trackMenuAnchorGlobalX = anchorGlobal.x
                    localX = playerPage.trackMenuAnchorGlobalX - playerOrigin.x - panelWidth / 2
                } else if (playerPage.trackMenuAnchorGlobalX >= 0) {
                    localX = playerPage.trackMenuAnchorGlobalX - playerOrigin.x - panelWidth / 2
                } else {
                    localX = playerPage.width / 2 - panelWidth / 2
                }
                localX = Math.max(margin, Math.min(localX, playerPage.width - panelWidth - margin))
                var localY = playerPage.height - playerPage.bottomChromeHeight - panelHeight - 12
                localY = Math.max(playerPage.topChromeHeight + 12, localY)
                x = Math.round(playerOrigin.x + localX)
                y = Math.round(playerOrigin.y + localY)
                width = Math.round(panelWidth)
                height = Math.round(panelHeight)
            }

            onVisibleChanged: {
                syncTrackMenuGeometry()
                if (visible) {
                    raise()
                    playerTrackMenuRoot.forceActiveFocus()
                }
            }

            Component.onCompleted: syncTrackMenuGeometry()

            Connections {
                target: root
                function onXChanged() { playerTrackMenuWindow.syncTrackMenuGeometry() }
                function onYChanged() { playerTrackMenuWindow.syncTrackMenuGeometry() }
                function onWidthChanged() { playerTrackMenuWindow.syncTrackMenuGeometry() }
                function onHeightChanged() { playerTrackMenuWindow.syncTrackMenuGeometry() }
                function onVisibilityChanged() { playerTrackMenuWindow.syncTrackMenuGeometry() }
                function onPlayerImmersiveChanged() { playerTrackMenuWindow.syncTrackMenuGeometry() }
            }

            Connections {
                target: playerPage
                function onWidthChanged() { playerTrackMenuWindow.syncTrackMenuGeometry() }
                function onHeightChanged() { playerTrackMenuWindow.syncTrackMenuGeometry() }
                function onTrackMenuModeChanged() { playerTrackMenuWindow.syncTrackMenuGeometry() }
                function onTrackMenuAnchorItemChanged() { playerTrackMenuWindow.syncTrackMenuGeometry() }
            }

            Connections {
                target: mpvVideo
                function onTracksChanged() { playerTrackMenuWindow.syncTrackMenuGeometry() }
            }

            Rectangle {
                id: playerTrackMenuRoot
                anchors.fill: parent
                focus: true
                radius: 10
                color: "#f20b0f16"
                border.color: "#667c8796"
                clip: true

                Keys.onPressed: function(event) {
                    if (event.key === Qt.Key_Escape) {
                        playerPage.closeTrackMenu()
                        event.accepted = true
                    } else {
                        playerPage.handlePlayerKey(event)
                    }
                }

                HoverHandler {
                    onHoveredChanged: if (hovered) playerPage.revealControls()
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 10

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            Label {
                                Layout.fillWidth: true
                                text: playerPage.trackMenuTitle()
                                color: "#ffffff"
                                font.pixelSize: 19
                                font.bold: true
                                elide: Text.ElideRight
                            }

                            MutedText {
                                Layout.fillWidth: true
                                text: playerPage.trackMenuHint()
                                color: "#aeb8c6"
                                elide: Text.ElideRight
                            }
                        }

                        Button {
                            id: closeTrackMenuButton
                            text: "X"
                            implicitWidth: 34
                            implicitHeight: 34
                            leftPadding: 0
                            rightPadding: 0
                            contentItem: Label {
                                text: closeTrackMenuButton.text
                                color: "#ffffff"
                                font.pixelSize: 18
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                radius: 8
                                color: closeTrackMenuButton.down ? "#4f8cff" : closeTrackMenuButton.hovered ? "#354253" : "#22313d"
                                border.color: closeTrackMenuButton.hovered ? "#6aa0ff" : "#405061"
                            }
                            onClicked: playerPage.closeTrackMenu()
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: "#334b5563"
                    }

                    ListView {
                        id: trackMenuList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        spacing: 6
                        model: playerPage.trackMenuMode === "subtitle"
                            ? mpvVideo.subtitleTracks
                            : playerPage.trackMenuMode === "speed"
                                ? playerPage.playbackSpeedOptions
                                : mpvVideo.audioTracks

                        header: Button {
                            id: subtitleOffItem
                            width: trackMenuList.width
                            height: playerPage.trackMenuMode === "subtitle" ? 42 : 0
                            visible: playerPage.trackMenuMode === "subtitle"
                            leftPadding: 12
                            rightPadding: 12

                            contentItem: RowLayout {
                                spacing: 10

                                Rectangle {
                                    Layout.preferredWidth: 22
                                    Layout.preferredHeight: 22
                                    radius: 11
                                    color: "#1a2430"
                                    border.color: "#465565"
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: t("player.subtitleOff")
                                    color: "#f4f7fb"
                                    font.pixelSize: 14
                                    elide: Text.ElideRight
                                    verticalAlignment: Text.AlignVCenter
                                }

                                Label {
                                    text: "OFF"
                                    color: "#93a4b8"
                                    font.pixelSize: 11
                                    font.bold: true
                                    verticalAlignment: Text.AlignVCenter
                                }
                            }

                            background: Rectangle {
                                radius: 8
                                color: subtitleOffItem.down ? "#34465a"
                                    : subtitleOffItem.hovered ? "#263341"
                                    : "#141b24"
                                border.color: subtitleOffItem.hovered ? "#4d6175" : "#25313d"
                            }

                            onClicked: {
                                mpvVideo.selectSubtitleTrack(-1)
                                playerPage.closeTrackMenu()
                            }
                        }

                        delegate: Button {
                            id: trackMenuItem
                            width: ListView.view.width
                            height: 42
                            leftPadding: 12
                            rightPadding: 12
                            property int trackIndex: index
                            property real speedValue: playerPage.trackMenuMode === "speed" ? modelData : 0
                            property bool speedMode: playerPage.trackMenuMode === "speed"
                            property bool selectedTrack: speedMode ? playerPage.speedSelected(speedValue) : model.selected
                            property string trackTitle: speedMode
                                ? playerPage.speedLabel(speedValue)
                                : (model.displayName ? model.displayName : "--")

                            contentItem: RowLayout {
                                spacing: 10

                                Rectangle {
                                    Layout.preferredWidth: 22
                                    Layout.preferredHeight: 22
                                    radius: 11
                                    color: trackMenuItem.selectedTrack ? "#4f8cff" : "#1a2430"
                                    border.color: trackMenuItem.selectedTrack ? "#78aaff" : "#465565"

                                    Label {
                                        anchors.centerIn: parent
                                        text: trackMenuItem.selectedTrack ? "✓" : ""
                                        color: "#ffffff"
                                        font.pixelSize: 13
                                        font.bold: true
                                    }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: trackMenuItem.trackTitle
                                    color: "#f4f7fb"
                                    font.pixelSize: 14
                                    font.bold: trackMenuItem.selectedTrack
                                    elide: Text.ElideRight
                                    verticalAlignment: Text.AlignVCenter
                                }

                                Label {
                                    visible: trackMenuItem.speedMode || (model.codec && model.codec.length > 0)
                                    text: trackMenuItem.speedMode
                                        ? (trackMenuItem.selectedTrack ? t("player.current") : "")
                                        : (model.codec ? model.codec.toUpperCase() : "")
                                    color: "#93a4b8"
                                    font.pixelSize: 11
                                    font.bold: true
                                    verticalAlignment: Text.AlignVCenter
                                }
                            }

                            background: Rectangle {
                                radius: 8
                                color: trackMenuItem.down ? "#34465a"
                                    : trackMenuItem.hovered ? "#263341"
                                    : trackMenuItem.selectedTrack ? "#253857"
                                    : "#141b24"
                                border.color: trackMenuItem.selectedTrack ? "#5d8ff2"
                                    : trackMenuItem.hovered ? "#4d6175"
                                    : "#25313d"
                            }

                            onClicked: {
                                if (playerPage.trackMenuMode === "subtitle") {
                                    mpvVideo.selectSubtitleTrack(trackIndex)
                                } else if (playerPage.trackMenuMode === "speed") {
                                    mpvVideo.setSpeed(speedValue)
                                } else {
                                    mpvVideo.selectAudioTrack(trackIndex)
                                }
                                playerPage.closeTrackMenu()
                            }
                        }
                    }
                }
            }
        }

        Window {
            id: playerIptvChannelWindow
            color: "transparent"
            flags: Qt.FramelessWindowHint | Qt.Tool
            transientParent: root
            visible: appViewModel.currentView === "player"
                && root.visible
                && playerPage.iptvChannelListVisible
                && appViewModel.iptvPlaybackActive

            function syncChannelListGeometry() {
                if (playerPage.width <= 0 || playerPage.height <= 0) {
                    return
                }
                var margin = 20
                var maxWidth = Math.max(220, playerPage.width - margin * 2)
                var panelWidth = Math.min(maxWidth, Math.min(430, Math.max(320, playerPage.width * 0.36)))
                var availableHeight = playerPage.height - playerPage.topChromeHeight - playerPage.bottomChromeHeight - margin * 2
                if (availableHeight < 220) {
                    availableHeight = Math.max(160, playerPage.height - margin * 2)
                }
                var panelHeight = Math.min(620, availableHeight)
                var playerOrigin = playerPage.mapToGlobal(0, 0)
                var localX = Math.max(margin, playerPage.width - panelWidth - margin)
                var preferredY = playerPage.topChromeHeight + margin
                var maxY = playerPage.height - playerPage.bottomChromeHeight - panelHeight - margin
                var localY = Math.max(margin, Math.min(preferredY, maxY))
                x = Math.round(playerOrigin.x + localX)
                y = Math.round(playerOrigin.y + localY)
                width = Math.round(panelWidth)
                height = Math.round(panelHeight)
            }

            onVisibleChanged: {
                syncChannelListGeometry()
                if (visible) {
                    raise()
                    playerIptvChannelRoot.forceActiveFocus()
                    Qt.callLater(playerPage.positionCurrentIptvChannel)
                }
            }

            Component.onCompleted: syncChannelListGeometry()

            Connections {
                target: root
                function onXChanged() { playerIptvChannelWindow.syncChannelListGeometry() }
                function onYChanged() { playerIptvChannelWindow.syncChannelListGeometry() }
                function onWidthChanged() { playerIptvChannelWindow.syncChannelListGeometry() }
                function onHeightChanged() { playerIptvChannelWindow.syncChannelListGeometry() }
                function onVisibilityChanged() { playerIptvChannelWindow.syncChannelListGeometry() }
                function onPlayerImmersiveChanged() { playerIptvChannelWindow.syncChannelListGeometry() }
            }

            Connections {
                target: playerPage
                function onWidthChanged() { playerIptvChannelWindow.syncChannelListGeometry() }
                function onHeightChanged() { playerIptvChannelWindow.syncChannelListGeometry() }
                function onTopChromeHeightChanged() { playerIptvChannelWindow.syncChannelListGeometry() }
                function onBottomChromeHeightChanged() { playerIptvChannelWindow.syncChannelListGeometry() }
            }

            Connections {
                target: appViewModel
                function onPlaybackChanged() {
                    if (!appViewModel.iptvPlaybackActive) {
                        playerPage.iptvChannelListVisible = false
                        return
                    }
                    if (playerIptvChannelWindow.visible) {
                        Qt.callLater(playerPage.positionCurrentIptvChannel)
                    }
                }
                function onIptvSearchTextChanged() { Qt.callLater(playerPage.positionCurrentIptvChannel) }
                function onIptvSelectedGroupChanged() { Qt.callLater(playerPage.positionCurrentIptvChannel) }
            }

            Rectangle {
                id: playerIptvChannelRoot
                anchors.fill: parent
                focus: true
                radius: 10
                color: "#f20b0f16"
                border.color: "#667c8796"
                clip: true

                Keys.onPressed: function(event) {
                    if (event.key === Qt.Key_Escape) {
                        playerPage.closeIptvChannelList()
                        event.accepted = true
                    } else {
                        playerPage.handlePlayerKey(event)
                    }
                }

                HoverHandler {
                    onHoveredChanged: if (hovered) playerPage.revealControls()
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 10

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            Label {
                                Layout.fillWidth: true
                                text: t("iptv.playerChannels")
                                color: "#ffffff"
                                font.pixelSize: 19
                                font.bold: true
                                elide: Text.ElideRight
                            }

                            MutedText {
                                Layout.fillWidth: true
                                text: appViewModel.iptvChannels.count + " " + t("iptv.channels")
                                color: "#aeb8c6"
                                elide: Text.ElideRight
                            }
                        }

                        Button {
                            id: closeIptvChannelsButton
                            text: "X"
                            implicitWidth: 34
                            implicitHeight: 34
                            leftPadding: 0
                            rightPadding: 0
                            contentItem: Label {
                                text: closeIptvChannelsButton.text
                                color: "#ffffff"
                                font.pixelSize: 18
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                radius: 8
                                color: closeIptvChannelsButton.down ? "#4f8cff" : closeIptvChannelsButton.hovered ? "#354253" : "#22313d"
                                border.color: closeIptvChannelsButton.hovered ? "#6aa0ff" : "#405061"
                            }
                            onClicked: playerPage.closeIptvChannelList()
                        }
                    }

                    ModernTextField {
                        Layout.fillWidth: true
                        placeholderText: t("iptv.search")
                        text: appViewModel.iptvSearchText
                        onTextChanged: appViewModel.iptvSearchText = text
                    }

                    ListView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: appViewModel.iptvGroups.length > 0 ? 38 : 0
                        visible: appViewModel.iptvGroups.length > 0
                        clip: true
                        orientation: ListView.Horizontal
                        boundsBehavior: Flickable.StopAtBounds
                        spacing: 8
                        model: appViewModel.iptvGroups

                        delegate: SeasonPill {
                            width: Math.min(160, Math.max(76, modelData.length * 8 + 32))
                            height: 34
                            title: modelData === "All" ? t("iptv.allGroups") : modelData
                            selected: modelData === appViewModel.iptvSelectedGroup
                            onActivated: appViewModel.selectIptvGroup(modelData)
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: "#334b5563"
                    }

                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        ListView {
                            id: playerIptvChannelList
                            anchors.fill: parent
                            clip: true
                            boundsBehavior: Flickable.StopAtBounds
                            spacing: 6
                            model: appViewModel.iptvChannels

                            delegate: Button {
                                id: iptvChannelItem
                                width: playerIptvChannelList.width
                                height: 58
                                leftPadding: 10
                                rightPadding: 10
                                property bool selectedChannel: model.channelId === appViewModel.currentIptvChannelId
                                property bool hasLogo: model.logoUrl && model.logoUrl.length > 0

                                contentItem: RowLayout {
                                    spacing: 10

                                    Rectangle {
                                        Layout.preferredWidth: 38
                                        Layout.preferredHeight: 38
                                        radius: 19
                                        color: iptvChannelItem.selectedChannel ? "#253857" : "#19232d"
                                        border.color: iptvChannelItem.selectedChannel ? "#78aaff" : "#3b4857"
                                        clip: true

                                        Image {
                                            id: playerChannelLogoImage
                                            anchors.fill: parent
                                            anchors.margins: 5
                                            source: iptvChannelItem.hasLogo ? model.logoUrl : ""
                                            fillMode: Image.PreserveAspectFit
                                            asynchronous: true
                                            visible: iptvChannelItem.hasLogo
                                        }

                                        ThumbnailLoadingIcon {
                                            anchors.centerIn: parent
                                            iconSize: 18
                                            running: iptvChannelItem.hasLogo && playerChannelLogoImage.status === Image.Loading
                                            accentColor: "#78aaff"
                                        }

                                        Label {
                                            anchors.centerIn: parent
                                            visible: !iptvChannelItem.hasLogo
                                            text: model.name && model.name.length > 0 ? model.name.charAt(0).toUpperCase() : "I"
                                            color: "#f4f7fb"
                                            font.pixelSize: 15
                                            font.bold: true
                                        }
                                    }

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2

                                        Label {
                                            Layout.fillWidth: true
                                            text: model.name
                                            color: "#f4f7fb"
                                            font.pixelSize: 14
                                            font.bold: iptvChannelItem.selectedChannel
                                            elide: Text.ElideRight
                                            verticalAlignment: Text.AlignVCenter
                                        }

                                        MutedText {
                                            Layout.fillWidth: true
                                            text: model.groupName && model.groupName.length > 0 ? model.groupName : t("iptv.allGroups")
                                            color: "#93a4b8"
                                            elide: Text.ElideRight
                                        }
                                    }

                                    Label {
                                        visible: iptvChannelItem.selectedChannel
                                        text: t("iptv.nowPlaying")
                                        color: "#9fc5ff"
                                        font.pixelSize: 11
                                        font.bold: true
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                }

                                background: Rectangle {
                                    radius: 8
                                    color: iptvChannelItem.down ? "#34465a"
                                        : iptvChannelItem.hovered ? "#263341"
                                        : iptvChannelItem.selectedChannel ? "#253857"
                                        : "#141b24"
                                    border.color: iptvChannelItem.selectedChannel ? "#5d8ff2"
                                        : iptvChannelItem.hovered ? "#4d6175"
                                        : "#25313d"
                                }

                                onClicked: {
                                    if (!selectedChannel) {
                                        appViewModel.playIptvChannel(index)
                                    }
                                    playerPage.revealControls()
                                    Qt.callLater(playerPage.raiseChromeWindows)
                                    Qt.callLater(playerPage.positionCurrentIptvChannel)
                                }
                            }
                        }

                        MutedText {
                            anchors.centerIn: parent
                            visible: appViewModel.iptvChannels.count === 0
                            text: t("iptv.noChannels")
                            color: "#aeb8c6"
                        }
                    }
                }
            }
        }

        Window {
            id: playerTopChromeWindow
            color: "transparent"
            flags: Qt.FramelessWindowHint | Qt.Tool
            transientParent: root
            visible: appViewModel.currentView === "player" && root.visible && (playerPage.controlsVisible || playerPage.exitConfirmVisible)

            function syncChromeGeometry() {
                if (playerPage.width <= 0 || playerPage.height <= 0) {
                    return
                }
                var origin = playerPage.mapToGlobal(0, 0)
                x = Math.round(origin.x)
                y = Math.round(origin.y)
                width = Math.max(1, Math.round(playerPage.width))
                height = playerPage.topChromeHeight
            }

            onVisibleChanged: {
                syncChromeGeometry()
                if (visible) {
                    if (playerLoadingWindow.visible) {
                        playerLoadingWindow.syncLoadingGeometry()
                        playerLoadingWindow.raise()
                    }
                    raise()
                    playerTopChromeRoot.forceActiveFocus()
                }
            }

            Component.onCompleted: syncChromeGeometry()

            Connections {
                target: root
                function onXChanged() { playerTopChromeWindow.syncChromeGeometry() }
                function onYChanged() { playerTopChromeWindow.syncChromeGeometry() }
                function onWidthChanged() { playerTopChromeWindow.syncChromeGeometry() }
                function onHeightChanged() { playerTopChromeWindow.syncChromeGeometry() }
                function onVisibilityChanged() { playerTopChromeWindow.syncChromeGeometry() }
                function onPlayerImmersiveChanged() { playerTopChromeWindow.syncChromeGeometry() }
            }

            Connections {
                target: playerPage
                function onWidthChanged() { playerTopChromeWindow.syncChromeGeometry() }
                function onHeightChanged() { playerTopChromeWindow.syncChromeGeometry() }
            }

            Item {
                id: playerTopChromeRoot
                anchors.fill: parent
                focus: true

                Keys.onPressed: function(event) {
                    playerPage.handlePlayerKey(event)
                }

                Rectangle {
                    id: playerTopControls
                    anchors.fill: parent
                    color: "#aa0a0d12"
                    border.color: "#44343b46"

                    HoverHandler {
                        onHoveredChanged: if (hovered) playerPage.revealControls()
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 20
                        anchors.rightMargin: 20
                        spacing: 12

                        ModernButton {
                            text: playerPage.exitConfirmVisible ? t("dialog.exitPlaybackTitle") : t("action.exitPlayback")
                            danger: true
                            onClicked: {
                                if (!playerPage.exitConfirmVisible) {
                                    playerPage.requestExitPlayback()
                                }
                            }
                        }

                        BodyText {
                            Layout.fillWidth: true
                            text: playerPage.exitConfirmVisible ? t("dialog.exitPlaybackPrompt") : appViewModel.selectedItemName
                            color: "#f4f7fb"
                            elide: Text.ElideRight
                        }

                        ModernButton {
                            visible: playerPage.exitConfirmVisible
                            text: t("action.cancel")
                            onClicked: playerPage.cancelExitPlayback()
                        }

                        ModernButton {
                            visible: playerPage.exitConfirmVisible
                            text: t("action.exitPlayback")
                            danger: true
                            onClicked: playerPage.confirmExitPlayback()
                        }

                        ModernButton {
                            visible: !playerPage.exitConfirmVisible
                            text: playerPage.immersive ? t("action.exitFullscreen") : t("action.fullscreen")
                            onClicked: playerPage.toggleFullscreen()
                        }
                    }
                }
            }
        }

        Window {
            id: playerBottomChromeWindow
            color: "transparent"
            flags: Qt.FramelessWindowHint | Qt.Tool
            transientParent: root
            visible: appViewModel.currentView === "player" && root.visible && (playerPage.controlsVisible || playerPage.exitConfirmVisible)

            function syncChromeGeometry() {
                if (playerPage.width <= 0 || playerPage.height <= 0) {
                    return
                }
                var origin = playerPage.mapToGlobal(0, playerPage.height - playerPage.bottomChromeHeight)
                x = Math.round(origin.x)
                y = Math.round(origin.y)
                width = Math.max(1, Math.round(playerPage.width))
                height = playerPage.bottomChromeHeight
            }

            onVisibleChanged: {
                syncChromeGeometry()
                if (visible) {
                    if (playerLoadingWindow.visible) {
                        playerLoadingWindow.syncLoadingGeometry()
                        playerLoadingWindow.raise()
                    }
                    raise()
                }
            }
            Component.onCompleted: syncChromeGeometry()

            Connections {
                target: root
                function onXChanged() { playerBottomChromeWindow.syncChromeGeometry() }
                function onYChanged() { playerBottomChromeWindow.syncChromeGeometry() }
                function onWidthChanged() { playerBottomChromeWindow.syncChromeGeometry() }
                function onHeightChanged() { playerBottomChromeWindow.syncChromeGeometry() }
                function onVisibilityChanged() { playerBottomChromeWindow.syncChromeGeometry() }
                function onPlayerImmersiveChanged() { playerBottomChromeWindow.syncChromeGeometry() }
            }

            Connections {
                target: playerPage
                function onWidthChanged() { playerBottomChromeWindow.syncChromeGeometry() }
                function onHeightChanged() { playerBottomChromeWindow.syncChromeGeometry() }
            }

            Item {
                id: playerBottomChromeRoot
                anchors.fill: parent
                focus: true

                Keys.onPressed: function(event) {
                    playerPage.handlePlayerKey(event)
                }

                Rectangle {
                    id: playerBottomControls
                    anchors.fill: parent
                    color: "#bb0a0d12"
                    border.color: "#44343b46"

                    HoverHandler {
                        onHoveredChanged: if (hovered) playerPage.revealControls()
                    }

            ColumnLayout {
                anchors.fill: parent
                anchors.leftMargin: 20
                anchors.rightMargin: 20
                anchors.topMargin: 12
                anchors.bottomMargin: 14
                spacing: 10

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    MutedText {
                        text: playerPage.formatTime(mpvVideo.position)
                        color: "#c7d0dd"
                    }

                    Slider {
                        id: progressSlider
                        Layout.fillWidth: true
                        from: 0
                        to: Math.max(1, mpvVideo.duration)
                        value: mpvVideo.position
                        onMoved: {
                            mpvVideo.seekAbsolute(value)
                            playerPage.beginSeekLoading()
                            appViewModel.reportPlaybackProgress(value, mpvVideo.paused)
                        }
                    }

                    MutedText {
                        text: playerPage.formatTime(mpvVideo.duration)
                        color: "#c7d0dd"
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    ModernButton {
                        text: t("action.rewind15")
                        onClicked: {
                            mpvVideo.seekRelative(-15)
                            playerPage.beginSeekLoading()
                            appViewModel.reportPlaybackProgress(Math.max(0, mpvVideo.position - 15), mpvVideo.paused)
                            playerPage.revealControls()
                        }
                    }

                    ModernButton {
                        text: mpvVideo.paused ? t("action.resume") : t("action.pause")
                        onClicked: {
                            mpvVideo.togglePause()
                            appViewModel.reportPlaybackProgress(mpvVideo.position, mpvVideo.paused)
                            playerPage.revealControls()
                        }
                    }

                    ModernButton {
                        text: t("action.forward15")
                        onClicked: {
                            mpvVideo.seekRelative(15)
                            playerPage.beginSeekLoading()
                            appViewModel.reportPlaybackProgress(mpvVideo.position + 15, mpvVideo.paused)
                            playerPage.revealControls()
                        }
                    }

                    ModernButton {
                        visible: appViewModel.iptvPlaybackActive
                        text: t("iptv.playerChannels")
                        onClicked: {
                            playerPage.openIptvChannelList()
                            playerPage.revealControls()
                        }
                    }

                    ModernButton {
                        id: subtitleTrackButton
                        text: t("player.subtitles")
                        enabled: mpvVideo.subtitleTracks.count > 0
                        onClicked: {
                            playerPage.openTrackMenu("subtitle", subtitleTrackButton)
                            playerPage.revealControls()
                        }
                    }

                    ModernButton {
                        id: audioTrackButton
                        text: t("player.audio")
                        enabled: mpvVideo.audioTracks.count > 1
                        onClicked: {
                            playerPage.openTrackMenu("audio", audioTrackButton)
                            playerPage.revealControls()
                        }
                    }

                    ModernButton {
                        id: speedButton
                        text: t("player.speed") + " " + playerPage.speedLabel(mpvVideo.speed)
                        onClicked: {
                            playerPage.openTrackMenu("speed", speedButton)
                            playerPage.revealControls()
                        }
                    }

                    ModernButton {
                        text: t("player.info")
                        onClicked: {
                            playerPage.closeIptvChannelList(false)
                            playerPage.videoInfoVisible = !playerPage.videoInfoVisible
                            playerPage.revealControls()
                            Qt.callLater(playerPage.raiseChromeWindows)
                        }
                    }

                    ModernButton {
                        text: t("player.cacheShort") + " " + playerPage.cacheDurationText(mpvVideo.cacheDurationSeconds)
                        onClicked: {
                            playerPage.closeIptvChannelList(false)
                            playerPage.videoInfoVisible = true
                            playerPage.revealControls()
                            Qt.callLater(playerPage.raiseChromeWindows)
                        }
                    }

                    BodyText {
                        text: t("player.volume")
                        color: "#f4f7fb"
                    }

                    Slider {
                        Layout.preferredWidth: 130
                        from: 0
                        to: 100
                        value: mpvVideo.volume
                        onMoved: mpvVideo.setVolume(value)
                    }

                    Item { Layout.fillWidth: true }
                }
            }
        }
            }
        }
    }

    component IptvPage: Flickable {
        id: iptvFlick
        contentWidth: width
        contentHeight: iptvColumn.implicitHeight
        clip: true

        ColumnLayout {
            id: iptvColumn
            width: iptvFlick.width
            spacing: 18

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                SectionHeader {
                    Layout.fillWidth: true
                    title: t("iptv.title")
                    subtitle: appViewModel.iptvChannels.count + " " + t("iptv.channels")
                }

                ModernTextField {
                    Layout.preferredWidth: Math.min(360, Math.max(220, iptvFlick.width * 0.34))
                    placeholderText: t("iptv.search")
                    text: appViewModel.iptvSearchText
                    onTextChanged: appViewModel.iptvSearchText = text
                }
            }

            ListView {
                Layout.fillWidth: true
                Layout.preferredHeight: appViewModel.iptvGroups.length > 0 ? 50 : 0
                visible: appViewModel.iptvGroups.length > 0
                clip: true
                orientation: ListView.Horizontal
                boundsBehavior: Flickable.StopAtBounds
                spacing: 10
                model: appViewModel.iptvGroups

                delegate: SeasonPill {
                    width: Math.min(190, Math.max(92, modelData.length * 9 + 34))
                    height: 40
                    title: modelData === "All" ? t("iptv.allGroups") : modelData
                    selected: modelData === appViewModel.iptvSelectedGroup
                    onActivated: appViewModel.selectIptvGroup(modelData)
                }
            }

            GridView {
                id: iptvGrid
                Layout.fillWidth: true
                Layout.preferredHeight: appViewModel.iptvChannels.count > 0
                    ? Math.ceil(appViewModel.iptvChannels.count / Math.max(1, Math.floor(width / 214))) * 182
                    : 120
                clip: true
                interactive: false
                model: appViewModel.iptvChannels
                cellWidth: Math.max(196, width / Math.max(1, Math.floor(width / 214)))
                cellHeight: 176

                delegate: IptvChannelCard {
                    width: iptvGrid.cellWidth - 14
                    height: 164
                    title: model.name
                    groupName: model.groupName
                    logoUrl: model.logoUrl
                    onActivated: appViewModel.playIptvChannel(index)
                }
            }

            MutedText {
                Layout.fillWidth: true
                visible: appViewModel.iptvChannels.count === 0 && !appViewModel.loading
                text: t("iptv.noChannels")
            }
        }
    }

    component WebDavPage: Item {
        id: webDavPage

        Flickable {
            id: webDavFlick
            anchors.fill: parent
            contentWidth: width
            contentHeight: webDavColumn.implicitHeight
            clip: true
            interactive: !appViewModel.loading

            ColumnLayout {
                id: webDavColumn
                width: webDavFlick.width
                spacing: 14

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    SectionHeader {
                        Layout.fillWidth: true
                        title: t("webdav.title")
                        subtitle: appViewModel.webDavCurrentPath
                    }

                    ModernButton {
                        text: t("action.backToServices")
                        onClicked: appViewModel.webDavBack()
                    }

                    ModernButton {
                        text: t("action.refresh")
                        onClicked: appViewModel.refreshWebDavDirectory()
                    }

                    ModernButton {
                        text: t("action.upload")
                        onClicked: appViewModel.chooseWebDavUploadFiles()
                    }

                    ModernButton {
                        text: t("action.uploadFolder")
                        onClicked: appViewModel.chooseWebDavUploadFolder()
                    }

                    ModernButton {
                        text: t("action.transfers") + (appViewModel.activeTransferCount > 0 ? " (" + appViewModel.activeTransferCount + ")" : "")
                        onClicked: appViewModel.openTransfers()
                    }
                }

                Item {
                    id: webDavListArea
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.max(180, appViewModel.webDavItems.count * 72)
                    clip: true

                    ListView {
                        anchors.fill: parent
                        enabled: !appViewModel.loading
                        opacity: appViewModel.loading ? 0.34 : 1
                        interactive: false
                        spacing: 10
                        model: appViewModel.webDavItems
                        delegate: WebDavFileRow {
                            width: ListView.view.width
                            title: model.name
                            subtitle: model.directory ? "Folder" : model.contentType + "  " + model.bytes + " B"
                            directory: model.directory
                            playable: model.playable
                            onActivated: appViewModel.openWebDavItem(index)
                            onDownloadRequested: appViewModel.downloadWebDavItem(index)
                        }
                        Behavior on opacity { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                    }

                    MutedText {
                        anchors.centerIn: parent
                        visible: appViewModel.webDavItems.count === 0 && !appViewModel.loading
                        text: t("webdav.empty")
                    }
                }
            }
        }

        Rectangle {
            id: webDavLoadingOverlay
            anchors.fill: parent
            visible: appViewModel.loading
            color: root.darkTheme ? "#d90f1217" : "#ddf5f7fb"
            z: 10

            MouseArea {
                anchors.fill: parent
            }

            Column {
                anchors.centerIn: parent
                width: Math.min(parent.width - 48, 360)
                spacing: 12

                BusyIndicator {
                    anchors.horizontalCenter: parent.horizontalCenter
                    running: webDavLoadingOverlay.visible
                    implicitWidth: 46
                    implicitHeight: 46
                }

                Label {
                    width: parent.width
                    text: t("webdav.loadingFolder")
                    color: root.darkTheme ? "#ffffff" : theme.text
                    font.pixelSize: 20
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                }

                Label {
                    width: parent.width
                    text: t("webdav.loadingHint")
                    color: root.darkTheme ? "#cbd5e1" : theme.muted
                    font.pixelSize: 13
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideRight
                }
            }
        }
    }

    component TransfersPage: Item {
        id: transfersPage
        property bool showingDetails: appViewModel.selectedTransferGroupId.length > 0
        property var visibleModel: showingDetails
            ? appViewModel.transferDetailTasks
            : appViewModel.transferTasks

        function filterLabel(filter) {
            switch (filter) {
            case "incomplete": return t("transfers.filterIncomplete")
            case "completed": return t("transfers.filterCompleted")
            case "failed": return t("transfers.filterFailed")
            case "canceled": return t("transfers.filterCanceled")
            default: return t("transfers.filterAll")
            }
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 16

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                IconButton {
                    visible: transfersPage.showingDetails
                    text: "\u2039"
                    onClicked: appViewModel.closeTransferGroup()
                    ToolTip.visible: hovered
                    ToolTip.text: t("transfers.title")
                }

                SectionHeader {
                    Layout.fillWidth: true
                    title: transfersPage.showingDetails
                        ? appViewModel.selectedTransferGroupTitle
                        : t("transfers.title")
                    subtitle: transfersPage.showingDetails
                        ? t("transfers.detailsSubtitle")
                        : t("transfers.subtitle")
                }
                ModernButton {
                    visible: !transfersPage.showingDetails
                        && appViewModel.completedTransferCount + appViewModel.failedTransferCount > 0
                    text: t("transfers.clearFinished")
                    onClicked: appViewModel.clearFinishedTransfers()
                }
                ModernButton {
                    text: t("action.backToServices")
                    onClicked: appViewModel.backToHome()
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 76
                spacing: 0

                TransferSummaryBlock {
                    label: t("transfers.pending")
                    value: appViewModel.activeTransferCount.toString()
                    valueColor: appViewModel.activeTransferCount > 0 ? theme.primary : theme.text
                }
                Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 40; color: theme.border }
                TransferSummaryBlock {
                    label: t("transfers.completed")
                    value: appViewModel.completedTransferCount.toString()
                    valueColor: appViewModel.completedTransferCount > 0 ? theme.success : theme.text
                }
                Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 40; color: theme.border }
                TransferSummaryBlock {
                    label: t("transfers.failed")
                    value: appViewModel.failedTransferCount.toString()
                    valueColor: appViewModel.failedTransferCount > 0 ? theme.danger : theme.text
                }
                Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 40; color: theme.border }
                TransferRateSummaryBlock {
                    label: t("transfers.speed")
                    downloadRate: appViewModel.transferDownloadBytesPerSecond
                    uploadRate: appViewModel.transferUploadBytesPerSecond
                }
                Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 54; color: theme.border }
                TransferRateSummaryBlock {
                    label: t("transfers.averageSpeed")
                    downloadRate: appViewModel.transferAverageDownloadBytesPerSecond
                    uploadRate: appViewModel.transferAverageUploadBytesPerSecond
                }
                Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 40; color: theme.border }
                TransferSummaryBlock {
                    label: t("transfers.remaining")
                    value: appViewModel.transferRemainingBytes >= 0
                        ? root.formatBytes(appViewModel.transferRemainingBytes)
                        : t("transfers.unknown")
                    valueColor: appViewModel.transferRemainingBytes > 0 ? theme.warning : theme.text
                }
            }

            Rectangle {
                visible: transfersPage.showingDetails
                Layout.fillWidth: true
                Layout.preferredHeight: 42
                radius: 8
                color: theme.input
                border.color: theme.border

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 4
                    spacing: 3

                    Repeater {
                        model: ["all", "incomplete", "completed", "failed", "canceled"]

                        delegate: TransferFilterButton {
                            Layout.fillWidth: true
                            text: transfersPage.filterLabel(modelData)
                            selected: appViewModel.transferDetailFilter === modelData
                            onClicked: appViewModel.transferDetailFilter = modelData
                        }
                    }
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                ListView {
                    id: transferList
                    anchors.fill: parent
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    spacing: 10
                    reuseItems: true
                    cacheBuffer: 300
                    model: transfersPage.visibleModel
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                    delegate: TransferTaskRow {
                        width: ListView.view.width
                        taskId: model.taskId
                        title: model.title
                        direction: model.direction
                        status: model.status
                        detail: model.detail
                        target: model.target
                        bytesDone: model.bytesDone
                        bytesTotal: model.bytesTotal
                        bytesPerSecond: model.bytesPerSecond
                        averageBytesPerSecond: model.averageBytesPerSecond
                        bytesRemaining: model.bytesRemaining
                        progress: model.progress
                        fileCount: model.fileCount
                        completedFileCount: model.completedFileCount
                        isGroup: model.isGroup
                        cancellable: model.cancellable
                        canPause: model.canPause
                        canResume: model.canResume
                        retryable: model.retryable
                        onActivated: {
                            if (model.isGroup) {
                                appViewModel.openTransferGroup(model.taskId)
                            }
                        }
                    }
                }

                ColumnLayout {
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 48, 360)
                    visible: transfersPage.visibleModel.count === 0
                    spacing: 8

                    Label {
                        Layout.fillWidth: true
                        text: "\u2193"
                        color: theme.subtle
                        font.pixelSize: 34
                        horizontalAlignment: Text.AlignHCenter
                    }
                    Label {
                        Layout.fillWidth: true
                        text: transfersPage.showingDetails
                            ? appViewModel.transferDetailFilter === "all"
                                ? t("transfers.emptyDetails")
                                : t("transfers.emptyFiltered")
                            : t("transfers.empty")
                        color: theme.text
                        font.pixelSize: 16
                        font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                    }
                    MutedText {
                        Layout.fillWidth: true
                        text: transfersPage.showingDetails
                            ? appViewModel.transferDetailFilter === "all"
                                ? t("transfers.detailsSubtitle")
                                : t("transfers.emptyFilteredHint")
                            : t("transfers.emptyHint")
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }

    component HistorySummaryCard: Rectangle {
        property string title: ""
        property string value: ""
        property string subtitle: ""
        property color accentColor: theme.primary

        Layout.fillWidth: true
        Layout.preferredHeight: 116
        radius: 10
        color: theme.elevated
        border.color: theme.border
        clip: true

        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 4
            color: accentColor
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.leftMargin: 18
            anchors.rightMargin: 16
            anchors.topMargin: 14
            anchors.bottomMargin: 14
            spacing: 6

            MutedText {
                Layout.fillWidth: true
                text: title
                elide: Text.ElideRight
            }

            Label {
                Layout.fillWidth: true
                text: value
                color: theme.text
                font.pixelSize: 27
                font.bold: true
                elide: Text.ElideRight
            }

            Item { Layout.fillHeight: true }

            MutedText {
                Layout.fillWidth: true
                text: subtitle
                color: theme.subtle
                elide: Text.ElideRight
            }
        }
    }

    component HistoryMetricBlock: ColumnLayout {
        property string label: ""
        property string value: ""
        property color valueColor: theme.text

        spacing: 2

        MutedText {
            Layout.fillWidth: true
            text: label
            color: theme.subtle
            font.pixelSize: 11
            elide: Text.ElideRight
        }

        Label {
            Layout.fillWidth: true
            text: value
            color: valueColor
            font.pixelSize: 14
            font.bold: true
            elide: Text.ElideRight
        }
    }

    component HistoryTrafficMetricBlock: ColumnLayout {
        property string label: ""
        property real bytesIn: 0
        property real bytesOut: 0
        property color valueColor: theme.text

        spacing: 2

        MutedText {
            Layout.fillWidth: true
            text: label
            color: theme.subtle
            font.pixelSize: 11
            elide: Text.ElideRight
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Label {
                Layout.fillWidth: true
                text: "↓ " + root.formatBytes(bytesIn)
                color: valueColor
                font.pixelSize: 13
                font.bold: true
                elide: Text.ElideRight
            }

            Label {
                Layout.fillWidth: true
                text: "↑ " + root.formatBytes(bytesOut)
                color: valueColor
                font.pixelSize: 13
                font.bold: true
                horizontalAlignment: Text.AlignRight
                elide: Text.ElideRight
            }
        }
    }

    component HistoryStatRow: Rectangle {
        property string date: ""
        property string serviceName: ""
        property string serviceType: ""
        property real watchSeconds: 0
        property real normalNetworkBytesIn: 0
        property real normalNetworkBytesOut: 0
        property real keepAliveNetworkBytesIn: 0
        property real keepAliveNetworkBytesOut: 0
        readonly property real normalNetworkBytesTotal: normalNetworkBytesIn + normalNetworkBytesOut
        readonly property real keepAliveNetworkBytesTotal: keepAliveNetworkBytesIn + keepAliveNetworkBytesOut
        readonly property real networkBytesInTotal: normalNetworkBytesIn + keepAliveNetworkBytesIn
        readonly property real networkBytesOutTotal: normalNetworkBytesOut + keepAliveNetworkBytesOut
        readonly property real networkBytesTotal: networkBytesInTotal + networkBytesOutTotal
        property bool privacyMode: false

        width: parent ? parent.width : 0
        implicitHeight: 92
        radius: 10
        color: rowMouse.containsMouse ? theme.elevatedHover : theme.elevated
        border.color: rowMouse.containsMouse ? theme.primary : theme.border

        MouseArea {
            id: rowMouse
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.NoButton
        }

        RowLayout {
            anchors.fill: parent
            anchors.margins: 14
            spacing: 14

            Rectangle {
                Layout.preferredWidth: 76
                Layout.preferredHeight: 50
                radius: 8
                color: theme.input
                border.color: theme.border

                Label {
                    anchors.centerIn: parent
                    text: root.formatHistoryDate(date)
                    color: theme.text
                    font.pixelSize: 15
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 160
                spacing: 6

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Label {
                        Layout.fillWidth: true
                        text: serviceName.length > 0 ? serviceName : t("history.service")
                        color: theme.text
                        font.pixelSize: 16
                        font.bold: true
                        elide: Text.ElideRight
                    }

                    Rectangle {
                        Layout.preferredHeight: 24
                        Layout.minimumWidth: 58
                        Layout.maximumWidth: 96
                        radius: 8
                        color: theme.input
                        border.color: theme.border

                        Label {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            text: serviceType
                            color: theme.muted
                            font.pixelSize: 11
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                    }

                    Rectangle {
                        visible: privacyMode
                        Layout.preferredHeight: 24
                        Layout.minimumWidth: 76
                        Layout.maximumWidth: 112
                        radius: 8
                        color: theme.primary
                        border.color: theme.primary

                        Label {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            text: t("history.privateBadge")
                            color: "#ffffff"
                            font.pixelSize: 11
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 14

                    HistoryMetricBlock {
                        Layout.fillWidth: true
                        label: t("history.watch")
                        value: root.formatDuration(watchSeconds)
                        valueColor: theme.primary
                    }

                    HistoryTrafficMetricBlock {
                        Layout.fillWidth: true
                        label: t("history.normalTraffic")
                        bytesIn: normalNetworkBytesIn
                        bytesOut: normalNetworkBytesOut
                    }

                    HistoryTrafficMetricBlock {
                        Layout.fillWidth: true
                        label: t("history.keepAliveTraffic")
                        bytesIn: keepAliveNetworkBytesIn
                        bytesOut: keepAliveNetworkBytesOut
                        valueColor: keepAliveNetworkBytesTotal > 0 ? theme.warning : theme.text
                    }
                }
            }

            ColumnLayout {
                Layout.preferredWidth: 176
                spacing: 3

                MutedText {
                    Layout.fillWidth: true
                    text: t("history.totalTraffic")
                    horizontalAlignment: Text.AlignRight
                    elide: Text.ElideRight
                }

                Label {
                    Layout.fillWidth: true
                    text: root.formatBytes(networkBytesTotal)
                    color: theme.text
                    font.pixelSize: 16
                    font.bold: true
                    horizontalAlignment: Text.AlignRight
                    elide: Text.ElideRight
                }

                MutedText {
                    Layout.fillWidth: true
                    text: root.formatTrafficSplit(networkBytesInTotal, networkBytesOutTotal)
                    horizontalAlignment: Text.AlignRight
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
            }
        }
    }

    component HistoryPage: Flickable {
        id: historyFlick
        contentWidth: width
        contentHeight: historyColumn.implicitHeight
        clip: true

        ColumnLayout {
            id: historyColumn
            width: historyFlick.width
            spacing: 16

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                SectionHeader {
                    Layout.fillWidth: true
                    title: t("history.title")
                    subtitle: t("history.retention")
                }

                ModernButton {
                    text: t("action.refresh")
                    enabled: !appViewModel.loading
                    onClicked: appViewModel.refreshHistoryStats()
                }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: historyFlick.width < 760 ? 1 : historyFlick.width < 1180 ? 2 : 3
                columnSpacing: 12
                rowSpacing: 12

                HistorySummaryCard {
                    title: t("history.totalWatch")
                    value: root.formatDuration(appViewModel.historyTotalWatchSeconds)
                    subtitle: t("history.watch")
                    accentColor: theme.primary
                }

                HistorySummaryCard {
                    title: t("history.totalDownload")
                    value: root.formatBytes(appViewModel.historyTotalNetworkBytesIn)
                    subtitle: t("history.traffic")
                    accentColor: theme.success
                }

                HistorySummaryCard {
                    title: t("history.totalUpload")
                    value: root.formatBytes(appViewModel.historyTotalNetworkBytesOut)
                    subtitle: t("history.traffic")
                    accentColor: theme.primary
                }

                HistorySummaryCard {
                    title: t("history.normalTraffic")
                    value: root.formatBytes(appViewModel.historyNormalNetworkBytes)
                    subtitle: root.formatTrafficSplit(appViewModel.historyNormalNetworkBytesIn,
                        appViewModel.historyNormalNetworkBytesOut)
                    accentColor: theme.success
                }

                HistorySummaryCard {
                    title: t("history.keepAliveTraffic")
                    value: root.formatBytes(appViewModel.historyKeepAliveNetworkBytes)
                    subtitle: root.formatTrafficSplit(appViewModel.historyKeepAliveNetworkBytesIn,
                        appViewModel.historyKeepAliveNetworkBytesOut)
                    accentColor: theme.warning
                }

                HistorySummaryCard {
                    title: t("history.totalTraffic")
                    value: root.formatBytes(appViewModel.historyTotalNetworkBytes)
                    subtitle: root.formatTrafficSplit(appViewModel.historyTotalNetworkBytesIn,
                        appViewModel.historyTotalNetworkBytesOut)
                    accentColor: theme.text
                }
            }

            SectionHeader {
                Layout.fillWidth: true
                title: t("history.dailyRecords")
                subtitle: appViewModel.privacyMode ? t("history.subtitlePrivacy") : t("history.subtitle")
            }

            ListView {
                id: historyList
                Layout.fillWidth: true
                Layout.preferredHeight: Math.max(180, contentHeight + 4)
                visible: appViewModel.usageStats.count > 0
                interactive: false
                spacing: 10
                clip: false
                model: appViewModel.usageStats
                section.property: "date"
                section.criteria: ViewSection.FullString
                section.delegate: Item {
                    required property string section
                    width: historyList.width
                    height: 34

                    Label {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        text: root.formatHistoryDate(section)
                        color: theme.muted
                        font.pixelSize: 13
                        font.bold: true
                        elide: Text.ElideRight
                    }
                }
                delegate: HistoryStatRow {
                    width: historyList.width
                    date: model.date
                    serviceName: model.serviceName
                    serviceType: model.serviceType
                    watchSeconds: model.watchSeconds
                    normalNetworkBytesIn: model.networkBytesIn
                    normalNetworkBytesOut: model.networkBytesOut
                    keepAliveNetworkBytesIn: model.keepAliveNetworkBytesIn
                    keepAliveNetworkBytesOut: model.keepAliveNetworkBytesOut
                    privacyMode: model.privacyMode
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 150
                visible: appViewModel.usageStats.count === 0
                radius: 10
                color: theme.elevated
                border.color: theme.border

                ColumnLayout {
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 48, 420)
                    spacing: 8

                    Label {
                        Layout.fillWidth: true
                        text: t("history.empty")
                        color: theme.text
                        font.pixelSize: 17
                        font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                    }

                    MutedText {
                        Layout.fillWidth: true
                        text: t("history.retention")
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }

    component ScheduledTasksPage: Flickable {
        id: scheduledTasksFlick
        contentWidth: width
        contentHeight: scheduledTasksColumn.implicitHeight
        clip: true

        function statusTitle() {
            switch (appViewModel.scheduledPlaybackStatus) {
            case "waiting": return t("schedule.statusWaiting")
            case "starting": return t("schedule.statusStarting")
            case "playing": return t("schedule.statusPlaying")
            case "completed": return t("schedule.statusCompleted")
            case "error": return t("schedule.statusError")
            default: return t("schedule.statusIdle")
            }
        }

        function statusColor() {
            switch (appViewModel.scheduledPlaybackStatus) {
            case "playing": return theme.success
            case "waiting": return theme.warning
            case "starting": return theme.primary
            case "error": return theme.danger
            default: return theme.subtle
            }
        }

        ColumnLayout {
            id: scheduledTasksColumn
            width: scheduledTasksFlick.width
            spacing: 20

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: statusContent.implicitHeight + 30
                radius: 8
                color: theme.surface
                border.color: scheduledTasksFlick.statusColor()

                ColumnLayout {
                    id: statusContent
                    anchors.fill: parent
                    anchors.margins: 15
                    spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        Rectangle {
                            Layout.preferredWidth: 12
                            Layout.preferredHeight: 12
                            radius: 6
                            color: scheduledTasksFlick.statusColor()
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3

                            Label {
                                Layout.fillWidth: true
                                text: scheduledTasksFlick.statusTitle()
                                color: theme.text
                                font.pixelSize: 17
                                font.bold: true
                                elide: Text.ElideRight
                            }

                            MutedText {
                                Layout.fillWidth: true
                                text: appViewModel.scheduledPlaybackStatus === "error"
                                    ? appViewModel.scheduledPlaybackError
                                    : appViewModel.scheduledPlaybackMediaName.length > 0
                                        ? appViewModel.scheduledPlaybackServerName + " · " + appViewModel.scheduledPlaybackMediaName
                                        : appViewModel.scheduledPlaybackServerName
                                visible: text.length > 0
                                elide: Text.ElideRight
                            }
                        }

                        ModernButton {
                            text: t("action.stop")
                            danger: true
                            visible: appViewModel.scheduledPlaybackActive || appViewModel.scheduledPlaybackWaiting
                            onClicked: appViewModel.stopScheduledPlayback()
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        visible: appViewModel.scheduledPlaybackTargetSeconds > 0

                        RowLayout {
                            Layout.fillWidth: true
                            MutedText { text: t("schedule.progress") }
                            Item { Layout.fillWidth: true }
                            MutedText {
                                text: appViewModel.formatDuration(appViewModel.scheduledPlaybackElapsedSeconds)
                                    + " / " + appViewModel.formatDuration(appViewModel.scheduledPlaybackTargetSeconds)
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 6
                            radius: 3
                            color: theme.border

                            Rectangle {
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                radius: 3
                                color: scheduledTasksFlick.statusColor()
                                width: parent.width * Math.min(1, appViewModel.scheduledPlaybackElapsedSeconds
                                    / Math.max(1, appViewModel.scheduledPlaybackTargetSeconds))
                            }
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                SectionHeader {
                    Layout.fillWidth: true
                    title: t("nav.scheduledTasks")
                    subtitle: appViewModel.scheduledPlaybackTasks.count + " " + t("schedule.savedConfigs")
                }

                ModernButton {
                    text: t("schedule.add")
                    enabled: appViewModel.scheduledEmbySources.count > 0
                    onClicked: {
                        appViewModel.beginAddScheduledPlaybackTask()
                        scheduledTaskEditorDialog.editing = false
                        scheduledTaskEditorDialog.open()
                    }
                }
            }

            ListView {
                id: scheduledTaskList
                Layout.fillWidth: true
                Layout.preferredHeight: count > 0 ? count * 140 : 0
                visible: count > 0
                interactive: false
                spacing: 10
                model: appViewModel.scheduledPlaybackTasks

                delegate: Rectangle {
                    width: scheduledTaskList.width
                    height: 130
                    radius: 8
                    color: taskMouse.hovered ? theme.elevatedHover : theme.surface
                    border.color: theme.border

                    HoverHandler { id: taskMouse }

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: 16

                        Rectangle {
                            Layout.preferredWidth: 4
                            Layout.fillHeight: true
                            radius: 2
                            color: model.scheduleType === "manual" || model.enabled ? theme.primary : theme.subtle
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 5

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                Label {
                                    Layout.fillWidth: true
                                    text: model.serverName
                                    color: theme.text
                                    font.pixelSize: 17
                                    font.bold: true
                                    elide: Text.ElideRight
                                }

                                Rectangle {
                                    visible: model.privateMode
                                    Layout.preferredHeight: 24
                                    Layout.minimumWidth: 76
                                    Layout.maximumWidth: 112
                                    radius: 8
                                    color: theme.primary
                                    border.color: theme.primary

                                    Label {
                                        anchors.fill: parent
                                        anchors.leftMargin: 8
                                        anchors.rightMargin: 8
                                        text: t("history.privateBadge")
                                        color: "#ffffff"
                                        font.pixelSize: 11
                                        font.bold: true
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                        elide: Text.ElideRight
                                    }
                                }

                                Rectangle {
                                    visible: model.scheduleType !== "manual"
                                    Layout.preferredHeight: 24
                                    Layout.minimumWidth: 68
                                    Layout.maximumWidth: 104
                                    radius: 8
                                    color: model.enabled ? root.withAlpha(theme.success, 0.16) : theme.elevated
                                    border.color: model.enabled ? root.withAlpha(theme.success, 0.62) : theme.border

                                    Label {
                                        anchors.fill: parent
                                        anchors.leftMargin: 8
                                        anchors.rightMargin: 8
                                        text: model.enabled ? t("schedule.enabledBadge") : t("schedule.disabledBadge")
                                        color: model.enabled ? theme.success : theme.muted
                                        font.pixelSize: 11
                                        font.bold: true
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                        elide: Text.ElideRight
                                    }
                                }
                            }

                            MutedText {
                                Layout.fillWidth: true
                                text: model.username + " · "
                                    + appViewModel.formatScheduledPlaybackSchedule(
                                        model.scheduleType, model.startTime, model.scheduleDays)
                                elide: Text.ElideRight
                            }

                            MutedText {
                                Layout.fillWidth: true
                                text: t("schedule.duration") + " · "
                                    + appViewModel.formatDuration(model.durationMinutes * 60)
                                elide: Text.ElideRight
                            }
                        }

                        ModernButton {
                            text: t("schedule.runNow")
                            enabled: !appViewModel.scheduledPlaybackActive && !appViewModel.scheduledPlaybackWaiting
                            onClicked: appViewModel.runScheduledPlaybackTaskNow(index)
                        }

                        ModernButton {
                            text: t("action.edit")
                            onClicked: {
                                appViewModel.editScheduledPlaybackTask(index)
                                scheduledTaskEditorDialog.editing = true
                                scheduledTaskEditorDialog.open()
                            }
                        }

                        ModernButton {
                            text: t("action.delete")
                            danger: true
                            onClicked: {
                                root.pendingScheduledDeleteRow = index
                                scheduledTaskDeleteDialog.open()
                            }
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 150
                visible: appViewModel.scheduledPlaybackTasks.count === 0
                radius: 8
                color: theme.surface
                border.color: theme.border

                ColumnLayout {
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 48, 460)
                    spacing: 8

                    Label {
                        Layout.fillWidth: true
                        text: appViewModel.scheduledEmbySources.count > 0 ? t("schedule.empty") : t("schedule.noSources")
                        color: theme.text
                        font.pixelSize: 17
                        font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }

    component ModernSpinBox: SpinBox {
        id: spin
        editable: true
        implicitWidth: 148
        implicitHeight: 40
        font.pixelSize: 14

        contentItem: TextInput {
            z: 2
            text: spin.textFromValue(spin.value, spin.locale)
            color: theme.text
            selectionColor: theme.primary
            selectedTextColor: "#ffffff"
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            readOnly: !spin.editable
            validator: spin.validator
            inputMethodHints: Qt.ImhDigitsOnly
        }

        up.indicator: Rectangle {
            x: spin.width - width
            height: spin.height
            implicitWidth: 38
            color: spin.up.pressed ? theme.primary : spin.up.hovered ? theme.elevatedHover : "transparent"
            Label {
                anchors.centerIn: parent
                text: "+"
                color: theme.text
                font.pixelSize: 18
                font.bold: true
            }
        }

        down.indicator: Rectangle {
            x: 0
            height: spin.height
            implicitWidth: 38
            color: spin.down.pressed ? theme.primary : spin.down.hovered ? theme.elevatedHover : "transparent"
            Label {
                anchors.centerIn: parent
                text: "−"
                color: theme.text
                font.pixelSize: 18
                font.bold: true
            }
        }

        background: Rectangle {
            radius: 8
            color: theme.input
            border.color: spin.activeFocus ? theme.primary : theme.border
        }
    }

    component SettingsPage: Flickable {
        id: settingsFlick
        property string oldPrivacyPin: ""
        property string newPrivacyPin: ""
        property string confirmPrivacyPin: ""
        contentWidth: width
        contentHeight: settingsColumn.implicitHeight
        clip: true

        function savePrivacyPin() {
            if (appViewModel.changePrivacyPin(settingsFlick.oldPrivacyPin, settingsFlick.newPrivacyPin, settingsFlick.confirmPrivacyPin)) {
                settingsFlick.oldPrivacyPin = ""
                settingsFlick.newPrivacyPin = ""
                settingsFlick.confirmPrivacyPin = ""
            }
        }

        ColumnLayout {
            id: settingsColumn
            width: Math.min(settingsFlick.width, 760)
            spacing: 18

            SettingsGroup {
                title: t("settings.appearance")

                SettingRow {
                    label: t("settings.theme")
                    ModernComboBox {
                        Layout.preferredWidth: 220
                        textRole: "label"
                        valueRole: "value"
                        model: [
                            { label: t("option.system"), value: "system" },
                            { label: t("option.dark"), value: "dark" },
                            { label: t("option.light"), value: "light" }
                        ]
                        currentIndex: appViewModel.themeMode === "system" ? 0 : appViewModel.themeMode === "dark" ? 1 : 2
                        onActivated: appViewModel.themeMode = model[index].value
                    }
                }

                SettingRow {
                    label: t("settings.language")
                    ModernComboBox {
                        Layout.preferredWidth: 220
                        textRole: "label"
                        valueRole: "value"
                        model: [
                            { label: t("option.system"), value: "system" },
                            { label: t("option.zh"), value: "zh_CN" },
                            { label: t("option.en"), value: "en_US" }
                        ]
                        currentIndex: appViewModel.languageMode === "system" ? 0 : appViewModel.languageMode === "zh_CN" ? 1 : 2
                        onActivated: appViewModel.languageMode = model[index].value
                    }
                }

                SettingRow {
                    label: t("settings.pageTransitions")
                    ModernCheckBox {
                        checked: appViewModel.pageTransitionsEnabled
                        onToggled: appViewModel.pageTransitionsEnabled = checked
                    }
                }
            }

            SettingsGroup {
                title: t("settings.desktop")

                SettingRow {
                    label: t("settings.minimizeToTray")
                    ModernCheckBox {
                        checked: appViewModel.minimizeToTray
                        enabled: trayController.trayAvailable
                        onToggled: appViewModel.minimizeToTray = checked
                    }
                }
            }

            SettingsGroup {
                title: t("settings.privacy")

                SettingRow {
                    label: t("settings.privacyPin")
                    ColumnLayout {
                        Layout.preferredWidth: 420
                        spacing: 8

                        MutedText {
                            Layout.fillWidth: true
                            text: appViewModel.privacyPinConfigured ? t("privacy.pinConfigured") : t("privacy.pinMissing")
                            elide: Text.ElideRight
                        }

                        ModernTextField {
                            Layout.fillWidth: true
                            visible: appViewModel.privacyPinConfigured
                            placeholderText: t("privacy.oldPin")
                            echoMode: TextInput.Password
                            inputMethodHints: Qt.ImhDigitsOnly
                            text: settingsFlick.oldPrivacyPin
                            onTextChanged: settingsFlick.oldPrivacyPin = text
                        }

                        ModernTextField {
                            Layout.fillWidth: true
                            placeholderText: t("privacy.newPin")
                            echoMode: TextInput.Password
                            inputMethodHints: Qt.ImhDigitsOnly
                            text: settingsFlick.newPrivacyPin
                            onTextChanged: settingsFlick.newPrivacyPin = text
                        }

                        ModernTextField {
                            Layout.fillWidth: true
                            placeholderText: t("privacy.confirmPin")
                            echoMode: TextInput.Password
                            inputMethodHints: Qt.ImhDigitsOnly
                            text: settingsFlick.confirmPrivacyPin
                            onTextChanged: settingsFlick.confirmPrivacyPin = text
                            onAccepted: settingsFlick.savePrivacyPin()
                        }

                        ModernButton {
                            id: privacyPinSaveButton
                            text: appViewModel.privacyPinConfigured ? t("privacy.changePin") : t("privacy.setPin")
                            onClicked: settingsFlick.savePrivacyPin()
                        }
                    }
                }
            }

            SettingsGroup {
                title: t("settings.webdav")

                SettingRow {
                    label: t("webdav.defaultDownload")
                    RowLayout {
                        Layout.preferredWidth: 420
                        spacing: 8
                        MutedText {
                            Layout.fillWidth: true
                            text: appViewModel.defaultDownloadDirectory.length > 0 ? appViewModel.defaultDownloadDirectory : t("webdav.noDownloadFolder")
                            elide: Text.ElideRight
                        }
                        ModernButton {
                            text: t("action.choose")
                            onClicked: appViewModel.chooseDefaultDownloadDirectory()
                        }
                    }
                }
            }
        }
    }

    component SettingsGroup: Rectangle {
        default property alias content: groupColumn.data
        property string title: ""
        Layout.fillWidth: true
        radius: 12
        color: theme.surface
        border.color: theme.border
        implicitHeight: groupColumn.implicitHeight + 28

        ColumnLayout {
            id: groupColumn
            anchors.fill: parent
            anchors.margins: 14
            spacing: 14

            Label {
                Layout.fillWidth: true
                text: title
                color: theme.text
                font.pixelSize: 17
                font.bold: true
            }
        }
    }

    component SettingRow: RowLayout {
        property string label: ""
        Layout.fillWidth: true
        spacing: 18

        BodyText {
            Layout.fillWidth: true
            text: label
        }
    }
}
