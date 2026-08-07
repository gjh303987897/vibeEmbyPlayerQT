import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Effects
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
    property int pendingTsslDeleteRow: -1
    property int pendingScheduledDeleteRow: -1
    property int dragFromRow: -1
    property bool playerImmersive: false
    property string downloadWarningTitle: ""
    property string downloadWarningMessage: ""
    property bool darkTheme: appViewModel.effectiveTheme !== "light"
    property bool useTraditionalMediaHome: appViewModel.currentView === "home"
        && ((appViewModel.serviceType === "Emby"
                && appViewModel.embyHomeLayout === "traditional")
            || (appViewModel.serviceType === "Jellyfin"
                && appViewModel.jellyfinHomeLayout === "traditional"))
    property bool useTraditionalPlayer: appViewModel.playerLayout === "traditional"
    property bool immersiveMediaHome: appViewModel.currentView === "home"
        && !root.useTraditionalMediaHome
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
        case "local":
            return Qt.rgba(0.180, 0.745, 0.690, 1.0)
        case "link":
            return Qt.rgba(0.376, 0.506, 0.941, 1.0)
        case "history":
        case "globalhistory":
            return Qt.rgba(0.910, 0.620, 0.220, 1.0)
        case "m3u8s":
            return Qt.rgba(0.055, 0.627, 0.447, 1.0)
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

    function formatPlaybackTime(seconds) {
        if (!Number.isFinite(seconds) || seconds < 0) {
            return "0:00"
        }
        var total = Math.floor(seconds)
        var hours = Math.floor(total / 3600)
        var minutes = Math.floor((total % 3600) / 60)
        var remainingSeconds = total % 60
        var minuteText = hours > 0 && minutes < 10 ? "0" + minutes : String(minutes)
        var secondText = remainingSeconds < 10 ? "0" + remainingSeconds : String(remainingSeconds)
        return hours > 0 ? hours + ":" + minuteText + ":" + secondText : minuteText + ":" + secondText
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
        onRejected: appViewModel.cancelPendingHistoryReplay()
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
        id: tsslDeleteDialog
        title: t("m3u8s.deleteTitle")
        standardButtons: Dialog.Ok | Dialog.Cancel
        width: Math.min(root.width - 64, 480)

        BodyText {
            width: parent.width
            text: t("m3u8s.deletePrompt")
            wrapMode: Text.WordWrap
        }

        onAccepted: {
            appViewModel.deleteManagedTssl(root.pendingTsslDeleteRow)
            root.pendingTsslDeleteRow = -1
        }
        onRejected: root.pendingTsslDeleteRow = -1
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
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        anchors.centerIn: parent
        padding: 0
        width: Math.min(root.width - 56, 720)
        height: Math.min(root.height - 72, 600)

        Overlay.modal: Rectangle {
            color: root.withAlpha("#000000", darkTheme ? 0.72 : 0.32)
        }

        enter: Transition {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 160; easing.type: Easing.OutCubic }
            NumberAnimation { property: "scale"; from: 0.97; to: 1; duration: 180; easing.type: Easing.OutCubic }
        }

        exit: Transition {
            NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 120; easing.type: Easing.InCubic }
            NumberAnimation { property: "scale"; from: 1; to: 0.98; duration: 120; easing.type: Easing.InCubic }
        }

        background: Rectangle {
            color: theme.surface
            radius: 22
            border.color: theme.border
            border.width: 1
        }

        contentItem: ColumnLayout {
            spacing: 0

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 92
                radius: 22
                color: darkTheme ? theme.elevated : theme.bg

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 1
                    color: theme.border
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 24
                    anchors.rightMargin: 18
                    spacing: 12

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 3

                        Label {
                            Layout.fillWidth: true
                            text: t("dialog.overviewTitle")
                            color: theme.text
                            font.pixelSize: 26
                            font.bold: true
                            elide: Text.ElideRight
                        }

                        Label {
                            Layout.fillWidth: true
                            text: appViewModel.selectedItemName
                            color: theme.muted
                            font.pixelSize: 13
                            elide: Text.ElideRight
                        }
                    }

                    Button {
                        id: overviewHeaderCloseButton
                        implicitWidth: 38
                        implicitHeight: 38
                        text: "\u00d7"
                        font.pixelSize: 22
                        font.bold: true
                        onClicked: overviewDialog.close()

                        contentItem: Label {
                            text: overviewHeaderCloseButton.text
                            color: overviewHeaderCloseButton.enabled ? theme.text : theme.subtle
                            font: overviewHeaderCloseButton.font
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        background: Rectangle {
                            radius: 19
                            color: overviewHeaderCloseButton.down
                                ? root.withAlpha(theme.primary, darkTheme ? 0.32 : 0.18)
                                : overviewHeaderCloseButton.hovered
                                    ? root.withAlpha(theme.primary, darkTheme ? 0.18 : 0.1)
                                    : "transparent"
                            border.color: overviewHeaderCloseButton.hovered ? theme.primary : theme.border
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
                Layout.topMargin: 22
                Layout.bottomMargin: 22
                clip: true

                ScrollBar.vertical: ScrollBar {
                    policy: ScrollBar.AsNeeded

                    contentItem: Rectangle {
                        implicitWidth: 6
                        radius: 3
                        color: parent.pressed
                            ? theme.primary
                            : parent.hovered
                                ? root.withAlpha(theme.primary, 0.72)
                                : root.withAlpha(theme.muted, darkTheme ? 0.48 : 0.34)
                    }
                }

                BodyText {
                    width: overviewScroll.availableWidth
                    text: appViewModel.selectedItemOverview.length > 0 ? appViewModel.selectedItemOverview : t("details.noOverview")
                    color: theme.text
                    font.pixelSize: 16
                    wrapMode: Text.WordWrap
                    lineHeight: 1.35
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 74
                radius: 22
                color: darkTheme ? theme.elevated : theme.bg

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: 1
                    color: theme.border
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 24
                    anchors.rightMargin: 24
                    spacing: 12

                    Label {
                        Layout.fillWidth: true
                        visible: appViewModel.selectedItemSeasonEpisode.length > 0
                        text: appViewModel.selectedItemSeasonEpisode
                        color: theme.muted
                        font.pixelSize: 13
                        elide: Text.ElideRight
                    }

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
                            radius: 10
                            color: overviewCloseButton.down
                                ? Qt.darker(theme.primary, 1.12)
                                : overviewCloseButton.hovered ? theme.primaryHover : theme.primary
                            border.color: overviewCloseButton.hovered ? theme.primaryHover : theme.primary
                        }
                    }
                }
            }
        }
    }

    FileDialog {
        id: externalSubtitleDialog
        title: t("player.selectSubtitleFile")
        fileMode: FileDialog.OpenFile
        nameFilters: [
            t("player.subtitleFiles") + " (*.srt *.ass *.ssa *.vtt *.sub *.idx *.sup *.smi *.sami *.lrc *.ttml *.dfxp)",
            t("player.allFiles") + " (*)"
        ]
        onAccepted: playerPageInstance.loadExternalSubtitle(selectedFile)
        onRejected: {
            if (appViewModel.currentView === "player") {
                playerPageInstance.revealControls()
            }
        }
    }

    header: ToolBar {
        height: root.playerImmersive || appViewModel.currentView === "details"
            || root.immersiveMediaHome ? 0 : 64
        visible: !root.playerImmersive && appViewModel.currentView !== "details"
            && !root.immersiveMediaHome
        enabled: visible
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
                    if (appViewModel.currentView === "history" || appViewModel.currentView === "globalHistory"
                            || appViewModel.currentView === "m3u8sManager") {
                        appViewModel.backToServices()
                    } else if (appViewModel.currentView === "scheduledTasks") {
                        appViewModel.backToServices()
                    } else if (appViewModel.currentView === "player" && appViewModel.webDavAudioPlaybackActive) {
                        appViewModel.minimizeWebDavAudioPlayer()
                    } else if (appViewModel.currentView === "webdav") {
                        appViewModel.webDavBack()
                    } else if (appViewModel.currentView === "local") {
                        appViewModel.localMediaBack()
                    } else if (appViewModel.currentView === "link") {
                        appViewModel.backToServices()
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
                Layout.fillWidth: !root.useTraditionalMediaHome
                Layout.preferredWidth: root.useTraditionalMediaHome ? 230 : -1
                Layout.maximumWidth: root.useTraditionalMediaHome ? 230 : 16777215

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
                            : appViewModel.currentView === "globalHistory" ? t("globalHistory.title")
                            : appViewModel.currentView === "m3u8sManager" ? t("m3u8s.title")
                            : appViewModel.currentView === "scheduledTasks" ? t("nav.scheduledTasks")
                            : appViewModel.currentView === "local" ? t("local.title")
                            : appViewModel.currentView === "link" ? t("link.title")
                            : appViewModel.currentView === "services" ? t("nav.services")
                            : appViewModel.currentView === "library" ? appViewModel.currentLibraryName
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
                        : appViewModel.currentView === "globalHistory" ? t("globalHistory.subtitle")
                        : appViewModel.currentView === "m3u8sManager" ? t("m3u8s.subtitle")
                        : appViewModel.currentView === "scheduledTasks" ? t("schedule.subtitle")
                        : appViewModel.currentView === "local" ? (appViewModel.localMediaDirectoryOpen ? appViewModel.localMediaCurrentPath : t("local.subtitle"))
                        : appViewModel.currentView === "link" ? t("link.subtitle")
                        : appViewModel.currentView === "iptv" ? appViewModel.currentUser
                        : appViewModel.loggedIn ? appViewModel.currentUser
                        : t("nav.chooseSource")
                    elide: Text.ElideRight
                }
            }

            BusyIndicator {
                running: appViewModel.loading || appViewModel.localMediaLoading
                visible: running
                implicitWidth: 28
                implicitHeight: 28
            }

            IconButton {
                width: 38
                height: 36
                text: "↻"
                visible: root.useTraditionalMediaHome
                enabled: !appViewModel.loading
                ToolTip.visible: hovered
                ToolTip.text: t("action.refresh")
                onClicked: appViewModel.refreshHome()
            }

            IconButton {
                width: 38
                height: 36
                text: "⚙"
                visible: root.useTraditionalMediaHome
                ToolTip.visible: hovered
                ToolTip.text: t("nav.settings")
                onClicked: appViewModel.openSettings()
            }

            MediaServerSearchBar {
                visible: appViewModel.serverSearchAvailable
                    && (appViewModel.currentView === "home" || appViewModel.currentView === "search")
                Layout.minimumWidth: visible ? (root.useTraditionalMediaHome ? 280 : 300) : 0
                Layout.preferredWidth: visible ? (root.useTraditionalMediaHome
                    ? 320 : Math.min(380, Math.max(320, root.width * 0.30))) : 0
                Layout.maximumWidth: visible ? Layout.preferredWidth : 0
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
                visible: (appViewModel.currentView === "home" && !root.useTraditionalMediaHome)
                    || appViewModel.currentView === "local"
                enabled: !appViewModel.loading && !appViewModel.localMediaLoading
                onClicked: {
                    if (appViewModel.currentView === "local") {
                        appViewModel.refreshLocalMediaDirectory()
                    } else {
                        appViewModel.refreshHome()
                    }
                }
            }

            ModernButton {
                text: t("local.addFolder")
                visible: appViewModel.currentView === "local"
                onClicked: appViewModel.chooseLocalMediaRoot()
            }

            ModernButton {
                text: t("nav.settings")
                visible: appViewModel.currentView !== "settings"
                    && !root.useTraditionalMediaHome
                onClicked: appViewModel.openSettings()
            }

            ModernButton {
                text: t("action.backToServices")
                visible: appViewModel.currentView !== "services"
                    && !root.useTraditionalMediaHome
                onClicked: appViewModel.backToServices()
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: theme.bg

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: root.playerImmersive || appViewModel.currentView === "details"
                || root.immersiveMediaHome ? 0 : 26
            spacing: root.playerImmersive || appViewModel.currentView === "details"
                || root.immersiveMediaHome ? 0 : 16

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
                readonly property int playerViewIndex: 5
                currentIndex: appViewModel.currentView === "services" ? 0
                    : appViewModel.currentView === "home" ? 1
                    : appViewModel.currentView === "library" ? 2
                    : appViewModel.currentView === "search" ? 3
                    : appViewModel.currentView === "details" ? 4
                    : appViewModel.currentView === "player" ? playerViewIndex
                    : appViewModel.currentView === "iptv" ? 6
                    : appViewModel.currentView === "webdav" ? 7
                    : appViewModel.currentView === "transfers" ? 8
                    : appViewModel.currentView === "history" ? 9
                    : appViewModel.currentView === "scheduledTasks" ? 10
                    : appViewModel.currentView === "local" ? 11
                    : appViewModel.currentView === "link" ? 12
                    : appViewModel.currentView === "globalHistory" ? 13
                    : appViewModel.currentView === "m3u8sManager" ? 14
                    : 15

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

                    // The embedded video surface and player chrome are native windows. They
                    // cannot follow a Qt Quick parent transform, so keep their global geometry
                    // stable while entering the player page.
                    if (nextIndex === pageStack.playerViewIndex) {
                        pageStack.resetTransition()
                        Qt.callLater(playerPageInstance.raiseChromeWindows)
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
                    id: servicePage
                    readonly property int cardColumnCount: width < 1080 ? 2 : 3
                    readonly property real cardSpacing: 16
                    readonly property real cardCellWidth: (width + cardSpacing) / cardColumnCount
                    readonly property real cardWidth: Math.max(0, cardCellWidth - cardSpacing)
                    readonly property real cardHeight: 156

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 16

                        GridLayout {
                            Layout.fillWidth: true
                            columns: servicePage.cardColumnCount
                            columnSpacing: servicePage.cardSpacing
                            rowSpacing: servicePage.cardSpacing

                            ServiceCard {
                                Layout.minimumWidth: servicePage.cardWidth
                                Layout.preferredWidth: servicePage.cardWidth
                                Layout.maximumWidth: servicePage.cardWidth
                                Layout.minimumHeight: servicePage.cardHeight
                                Layout.preferredHeight: servicePage.cardHeight
                                Layout.maximumHeight: servicePage.cardHeight
                                editing: false
                                serviceName: t("local.title")
                                serviceType: "Local"
                                host: t("local.subtitle")
                                leadingStatusText: t("local.builtIn")
                                leadingStatusColor: theme.success
                                trailingStatusText: t("local.folderCount").arg(appViewModel.localMediaRoots.count)
                                trailingStatusColor: theme.primary
                                onActivated: appViewModel.openLocalMedia()
                            }

                            ServiceCard {
                                Layout.minimumWidth: servicePage.cardWidth
                                Layout.preferredWidth: servicePage.cardWidth
                                Layout.maximumWidth: servicePage.cardWidth
                                Layout.minimumHeight: servicePage.cardHeight
                                Layout.preferredHeight: servicePage.cardHeight
                                Layout.maximumHeight: servicePage.cardHeight
                                editing: false
                                serviceName: t("link.title")
                                serviceType: "Link"
                                host: t("link.subtitle")
                                leadingStatusText: t("local.builtIn")
                                leadingStatusColor: theme.success
                                trailingStatusText: t("link.protocols")
                                trailingStatusColor: root.serviceAccentColor("Link")
                                onActivated: appViewModel.openLinkPlayback()
                            }

                            ServiceCard {
                                Layout.minimumWidth: servicePage.cardWidth
                                Layout.preferredWidth: servicePage.cardWidth
                                Layout.maximumWidth: servicePage.cardWidth
                                Layout.minimumHeight: servicePage.cardHeight
                                Layout.preferredHeight: servicePage.cardHeight
                                Layout.maximumHeight: servicePage.cardHeight
                                editing: false
                                serviceName: t("globalHistory.title")
                                serviceType: "History"
                                host: t("globalHistory.cardSubtitle")
                                leadingStatusText: t("globalHistory.builtIn")
                                leadingStatusColor: theme.success
                                trailingStatusText: t("globalHistory.localIndex")
                                trailingStatusColor: root.serviceAccentColor("History")
                                onActivated: appViewModel.openGlobalHistory()
                            }

                            ServiceCard {
                                Layout.minimumWidth: servicePage.cardWidth
                                Layout.preferredWidth: servicePage.cardWidth
                                Layout.maximumWidth: servicePage.cardWidth
                                Layout.minimumHeight: servicePage.cardHeight
                                Layout.preferredHeight: servicePage.cardHeight
                                Layout.maximumHeight: servicePage.cardHeight
                                editing: false
                                serviceName: t("m3u8s.title")
                                serviceType: "M3u8s"
                                host: t("m3u8s.cardSubtitle")
                                leadingStatusText: t("m3u8s.builtIn")
                                leadingStatusColor: theme.success
                                trailingStatusText: t("m3u8s.packageCount").arg(appViewModel.tsslPackages.count)
                                trailingStatusColor: root.serviceAccentColor("M3u8s")
                                onActivated: appViewModel.openM3u8sManager()
                            }
                        }

                        Item {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true

                            GridView {
                                id: serviceGrid
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: parent.width + servicePage.cardSpacing
                                clip: true
                                model: appViewModel.services
                                cellWidth: servicePage.cardCellWidth
                                cellHeight: servicePage.cardHeight + servicePage.cardSpacing
                                displaced: Transition {
                                    NumberAnimation { properties: "x,y"; duration: 160; easing.type: Easing.OutCubic }
                                }

                                delegate: ServiceCard {
                                    width: servicePage.cardWidth
                                    height: servicePage.cardHeight
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
                                    font.pixelSize: 20
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
                    }
                }

                Item {
                    id: homePage
                    property bool trendyLayout: !root.useTraditionalMediaHome
                    property var featuredModel: appViewModel.recommendedItems.count > 0
                        ? appViewModel.recommendedItems : appViewModel.continueItems
                    property bool showingRecommendations: appViewModel.recommendedItems.count > 0
                    property int featuredCount: featuredModel ? featuredModel.count : 0
                    property bool showInitialLoading: appViewModel.homeLoading
                        && appViewModel.continueItems.count === 0
                        && appViewModel.recommendedItems.count === 0
                        && appViewModel.libraries.count === 0
                    property int featuredIndex: 0

                    Timer {
                        interval: 10000
                        repeat: true
                        running: homePage.visible && homePage.trendyLayout
                            && homePage.featuredCount > 1
                        onTriggered: {
                            homePage.featuredIndex = (homePage.featuredIndex + 1)
                                % Math.min(8, homePage.featuredCount)
                        }
                    }

                    Connections {
                        target: homePage.featuredModel
                        function onCountChanged() {
                            if (homePage.featuredCount <= 0) {
                                homePage.featuredIndex = 0
                            } else if (homePage.featuredIndex >= Math.min(8, homePage.featuredCount)) {
                                homePage.featuredIndex = 0
                            }
                        }
                    }

                    Popup {
                        id: homeSearchPopup
                        x: {
                            var buttonPosition = homeSearchButton.mapToItem(homePage, 0, 0)
                            return Math.max(16,
                                buttonPosition.x + homeSearchButton.width - width)
                        }
                        y: {
                            var buttonPosition = homeSearchButton.mapToItem(homePage, 0, 0)
                            return buttonPosition.y + (homeSearchButton.height - height) / 2
                        }
                        readonly property real expandedWidth: Math.min(460,
                            Math.max(240, homePage.width - 48))
                        readonly property real expandedHeight: 66
                        width: expandedWidth
                        height: expandedHeight
                        padding: 12
                        clip: true
                        modal: false
                        focus: true
                        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                        z: 30

                        enter: Transition {
                            ParallelAnimation {
                                NumberAnimation {
                                    property: "opacity"
                                    from: 0
                                    to: 1
                                    duration: 220
                                    easing.type: Easing.OutCubic
                                }
                                NumberAnimation {
                                    property: "width"
                                    from: homeSearchButton.width
                                    to: homeSearchPopup.expandedWidth
                                    duration: 220
                                    easing.type: Easing.OutCubic
                                }
                                NumberAnimation {
                                    property: "height"
                                    from: homeSearchButton.height
                                    to: homeSearchPopup.expandedHeight
                                    duration: 220
                                    easing.type: Easing.OutCubic
                                }
                            }
                        }

                        exit: Transition {
                            ParallelAnimation {
                                NumberAnimation {
                                    property: "opacity"
                                    from: 1
                                    to: 0
                                    duration: 170
                                    easing.type: Easing.InCubic
                                }
                                NumberAnimation {
                                    property: "width"
                                    from: homeSearchPopup.expandedWidth
                                    to: homeSearchButton.width
                                    duration: 150
                                    easing.type: Easing.InCubic
                                }
                                NumberAnimation {
                                    property: "height"
                                    from: homeSearchPopup.expandedHeight
                                    to: homeSearchButton.height
                                    duration: 150
                                    easing.type: Easing.InCubic
                                }
                            }
                        }

                        background: Rectangle {
                            radius: 14
                            color: root.withAlpha(theme.surface, darkTheme ? 0.97 : 0.99)
                            border.color: root.withAlpha(theme.primary, 0.52)
                            border.width: 1
                        }

                        contentItem: MediaServerSearchBar {}

                        onOpened: Qt.callLater(function() {
                            if (homeSearchPopup.contentItem) {
                                homeSearchPopup.contentItem.focusInput()
                            }
                        })
                    }

                    Connections {
                        target: appViewModel
                        function onCurrentViewChanged() {
                            if (homeSearchPopup.visible && appViewModel.currentView !== "home") {
                                homeSearchPopup.close()
                            }
                        }
                    }

                    Flickable {
                        id: homeFlick
                        anchors.fill: parent
                        visible: homePage.trendyLayout
                        enabled: visible
                        contentWidth: width
                        contentHeight: homeContent.height
                        clip: true
                        opacity: homePage.showInitialLoading ? 0.24 : 1
                        boundsBehavior: Flickable.StopAtBounds

                        Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }

                        Item {
                            id: homeContent
                            width: homeFlick.width
                            height: homeHero.height + homeSections.implicitHeight + 42

                            Rectangle {
                                id: homeHero
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.top: parent.top
                                height: Math.max(390, Math.min(520, homePage.height * 0.63))
                                color: theme.surface
                                clip: true

                                ListView {
                                    id: heroList
                                    anchors.fill: parent
                                    orientation: ListView.Horizontal
                                    interactive: false
                                    model: homePage.featuredModel
                                    currentIndex: homePage.featuredIndex
                                    cacheBuffer: width
                                    onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Beginning)

                                    delegate: Item {
                                        width: heroList.width
                                        height: heroList.height

                                        Image {
                                            anchors.fill: parent
                                            source: model.backdropImageUrl.length > 0
                                                ? model.backdropImageUrl : model.continueImageUrl
                                            fillMode: Image.PreserveAspectCrop
                                            asynchronous: true
                                            cache: true
                                        }

                                        Rectangle {
                                            anchors.fill: parent
                                            gradient: Gradient {
                                                orientation: Gradient.Horizontal
                                                GradientStop { position: 0.0; color: "#e60a0d12" }
                                                GradientStop { position: 0.46; color: "#6b0a0d12" }
                                                GradientStop { position: 1.0; color: "#260a0d12" }
                                            }
                                        }

                                        Rectangle {
                                            anchors.fill: parent
                                            gradient: Gradient {
                                                GradientStop { position: 0.0; color: "#3d0a0d12" }
                                                GradientStop { position: 0.55; color: "#120a0d12" }
                                                GradientStop { position: 1.0; color: theme.bg }
                                            }
                                        }

                                        ColumnLayout {
                                            anchors.left: parent.left
                                            anchors.bottom: parent.bottom
                                            anchors.leftMargin: Math.max(34, parent.width * 0.055)
                                            anchors.bottomMargin: 48
                                            width: Math.min(650, parent.width * 0.58)
                                            spacing: 12

                                            Label {
                                                Layout.fillWidth: true
                                                text: model.seriesName.length > 0 ? model.seriesName : model.name
                                                color: "#ffffff"
                                                font.pixelSize: 36
                                                font.bold: true
                                                wrapMode: Text.WordWrap
                                                maximumLineCount: 2
                                                elide: Text.ElideRight
                                            }

                                            Label {
                                                Layout.fillWidth: true
                                                visible: model.name.length > 0 && model.seriesName.length > 0
                                                    && model.name !== model.seriesName
                                                text: model.name
                                                color: "#edf1f6"
                                                font.pixelSize: 17
                                                font.bold: true
                                                elide: Text.ElideRight
                                            }

                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: 9

                                                Label {
                                                    visible: model.communityRating.length > 0
                                                    text: "★ " + model.communityRating
                                                    color: "#ffcf66"
                                                    font.pixelSize: 14
                                                    font.bold: true
                                                }
                                                Label {
                                                    visible: model.productionYear.length > 0
                                                    text: model.productionYear
                                                    color: "#e7ebf1"
                                                    font.pixelSize: 14
                                                }
                                                Label {
                                                    visible: model.officialRating.length > 0
                                                    text: model.officialRating
                                                    color: "#e7ebf1"
                                                    font.pixelSize: 12
                                                    leftPadding: 7
                                                    rightPadding: 7
                                                    background: Rectangle {
                                                        radius: 4
                                                        color: "#33000000"
                                                        border.color: "#99ffffff"
                                                    }
                                                }
                                                Label {
                                                    visible: model.runTime.length > 0
                                                    text: model.runTime
                                                    color: "#e7ebf1"
                                                    font.pixelSize: 14
                                                }
                                                Label {
                                                    visible: appViewModel.formatSeasonEpisode(
                                                        model.parentIndexNumber, model.indexNumber).length > 0
                                                    text: appViewModel.formatSeasonEpisode(
                                                        model.parentIndexNumber, model.indexNumber)
                                                    color: "#e7ebf1"
                                                    font.pixelSize: 14
                                                }
                                            }

                                            Label {
                                                Layout.fillWidth: true
                                                visible: model.overview.length > 0
                                                text: model.overview
                                                color: "#d4d9e0"
                                                font.pixelSize: 14
                                                lineHeight: 1.18
                                                wrapMode: Text.WordWrap
                                                maximumLineCount: 3
                                                elide: Text.ElideRight
                                            }

                                            ModernButton {
                                                Layout.preferredWidth: 142
                                                text: "▶  " + t(homePage.showingRecommendations
                                                    ? "action.play" : "action.continue")
                                                font.bold: true
                                                onClicked: {
                                                    if (homePage.showingRecommendations) {
                                                        appViewModel.openRecommendedItem(index)
                                                    } else {
                                                        appViewModel.openContinueItem(index)
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }

                                RowLayout {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    anchors.leftMargin: 24
                                    anchors.rightMargin: 24
                                    height: 82
                                    spacing: 16

                                    Button {
                                        id: serverHomeButton
                                        Layout.preferredWidth: Math.min(220, implicitWidth)
                                        Layout.preferredHeight: 46
                                        leftPadding: 14
                                        rightPadding: 16
                                        text: appViewModel.currentServerName
                                        onClicked: appViewModel.backToServices()

                                        contentItem: RowLayout {
                                            spacing: 8

                                            Label {
                                                text: "\u2190"
                                                color: "#ffffff"
                                                font.pixelSize: 19
                                                font.bold: true
                                                Layout.alignment: Qt.AlignVCenter
                                            }

                                            Rectangle {
                                                Layout.preferredWidth: 26
                                                Layout.preferredHeight: 26
                                                radius: 6
                                                color: root.serviceAccentColor(appViewModel.serviceType)

                                                Label {
                                                    anchors.centerIn: parent
                                                    text: "▶"
                                                    color: "#ffffff"
                                                    font.pixelSize: 11
                                                    font.bold: true
                                                }
                                            }

                                            Label {
                                                Layout.fillWidth: true
                                                text: serverHomeButton.text
                                                color: "#ffffff"
                                                font.pixelSize: 14
                                                font.bold: true
                                                elide: Text.ElideRight
                                            }
                                        }

                                        background: Rectangle {
                                            radius: 8
                                            color: serverHomeButton.hovered ? "#4dffffff" : "#2effffff"
                                            border.color: "#55ffffff"
                                        }

                                        ToolTip.visible: hovered
                                        ToolTip.text: t("action.backToServices")
                                        Accessible.name: t("action.backToServices")
                                    }

                                    Item { Layout.fillWidth: true }

                                    HeroToolbarButton {
                                        id: homeSearchButton
                                        visible: appViewModel.serverSearchAvailable
                                        iconText: "\uD83D\uDD0D"
                                        text: t("search.action")
                                        ToolTip.visible: hovered
                                        ToolTip.text: t("search.action")
                                        Accessible.name: t("search.action")
                                        onClicked: homeSearchPopup.open()
                                    }

                                    HeroToolbarButton {
                                        iconText: "\u21BB"
                                        text: t("action.refresh")
                                        enabled: !appViewModel.loading
                                        ToolTip.visible: hovered
                                        ToolTip.text: t("action.refresh")
                                        Accessible.name: t("action.refresh")
                                        onClicked: appViewModel.refreshHome()
                                    }
                                }

                                Row {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    anchors.bottom: parent.bottom
                                    anchors.bottomMargin: 22
                                    spacing: 8
                                    visible: homePage.featuredCount > 1

                                    Repeater {
                                        model: Math.min(8, homePage.featuredCount)

                                        Rectangle {
                                            width: index === homePage.featuredIndex ? 22 : 7
                                            height: 7
                                            radius: 4
                                            color: index === homePage.featuredIndex ? "#ffffff" : "#7affffff"

                                            Behavior on width { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }

                                            MouseArea {
                                                anchors.fill: parent
                                                anchors.margins: -5
                                                onClicked: homePage.featuredIndex = index
                                            }
                                        }
                                    }
                                }

                                ColumnLayout {
                                    anchors.centerIn: parent
                                    visible: homePage.featuredCount === 0 && !homePage.showInitialLoading
                                    spacing: 10

                                    Label {
                                        Layout.alignment: Qt.AlignHCenter
                                        text: appViewModel.currentServerName
                                        color: "#ffffff"
                                        font.pixelSize: 34
                                        font.bold: true
                                    }

                                    Label {
                                        Layout.alignment: Qt.AlignHCenter
                                        text: t("section.noProgress")
                                        color: "#d4d9e0"
                                        font.pixelSize: 15
                                    }
                                }
                            }

                            Column {
                                id: homeSections
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.top: homeHero.bottom
                                anchors.leftMargin: 26
                                anchors.rightMargin: 26
                                topPadding: 12
                                spacing: 30

                                Column {
                                    width: parent.width
                                    spacing: 12

                                    RowLayout {
                                        width: parent.width
                                        height: 34

                                        Label {
                                            Layout.fillWidth: true
                                            text: t("section.continueWatching")
                                            color: theme.text
                                            font.pixelSize: 21
                                            font.bold: true
                                        }

                                        Label {
                                            visible: appViewModel.continueItems.count > 0
                                            text: appViewModel.continueItems.count
                                            color: theme.muted
                                            font.pixelSize: 14
                                        }
                                    }

                                    Item {
                                        id: continueRail
                                        width: parent.width
                                        height: appViewModel.continueItems.count > 0 ? 208 : 54

                                        function maxContentX() {
                                            return Math.max(0, continueList.contentWidth - continueList.width)
                                        }

                                        function scrollBy(delta) {
                                            continueList.contentX = Math.max(0,
                                                Math.min(maxContentX(), continueList.contentX + delta))
                                        }

                                        ListView {
                                            id: continueList
                                            anchors.fill: parent
                                            clip: true
                                            orientation: ListView.Horizontal
                                            boundsBehavior: Flickable.StopAtBounds
                                            spacing: 16
                                            model: appViewModel.continueItems

                                            delegate: ContinueWatchingCard {
                                                width: Math.min(306, Math.max(252, continueList.width * 0.28))
                                                height: 204
                                                title: model.name.length > 0 ? model.name : model.seriesName
                                                seriesName: model.itemType === "Episode" ? model.seriesName : ""
                                                seasonEpisode: appViewModel.formatSeasonEpisode(
                                                    model.parentIndexNumber, model.indexNumber)
                                                progressText: appViewModel.formatContinueProgress(model.playedPercentage)
                                                imageUrl: model.continueImageUrl
                                                backdropUrl: model.backdropImageUrl
                                                progress: model.playedPercentage
                                                onActivated: appViewModel.openContinueItem(index)
                                            }

                                            WheelHandler {
                                                onWheel: function(event) {
                                                    var horizontalDelta = event.angleDelta.x !== 0
                                                        ? -event.angleDelta.x
                                                        : ((event.modifiers & Qt.ShiftModifier)
                                                            ? -event.angleDelta.y : 0)
                                                    if (horizontalDelta !== 0) {
                                                        continueRail.scrollBy(horizontalDelta)
                                                        event.accepted = true
                                                    } else if (event.angleDelta.y !== 0) {
                                                        var maxContentY = Math.max(0,
                                                            homeFlick.contentHeight - homeFlick.height)
                                                        homeFlick.contentY = Math.max(0,
                                                            Math.min(maxContentY,
                                                                homeFlick.contentY - event.angleDelta.y))
                                                        event.accepted = true
                                                    } else {
                                                        event.accepted = false
                                                    }
                                                }
                                            }
                                        }

                                        IconButton {
                                            anchors.left: parent.left
                                            anchors.verticalCenter: parent.verticalCenter
                                            anchors.leftMargin: 8
                                            visible: enabled && appViewModel.continueItems.count > 0
                                            text: "‹"
                                            enabled: continueList.contentX > 1
                                            onClicked: continueRail.scrollBy(-Math.max(320, continueList.width * 0.78))
                                        }

                                        IconButton {
                                            anchors.right: parent.right
                                            anchors.verticalCenter: parent.verticalCenter
                                            anchors.rightMargin: 8
                                            visible: enabled && appViewModel.continueItems.count > 0
                                            text: "›"
                                            enabled: continueList.contentX < continueRail.maxContentX() - 1
                                            onClicked: continueRail.scrollBy(Math.max(320, continueList.width * 0.78))
                                        }

                                        MutedText {
                                            anchors.left: parent.left
                                            anchors.top: parent.top
                                            visible: appViewModel.continueItems.count === 0 && !homePage.showInitialLoading
                                            text: t("section.noProgress")
                                        }
                                    }
                                }

                                Column {
                                    id: librarySection
                                    width: parent.width
                                    spacing: 12

                                    RowLayout {
                                        width: parent.width
                                        height: 34

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 1

                                            Label {
                                                Layout.fillWidth: true
                                                text: t("section.libraries")
                                                color: theme.text
                                                font.pixelSize: 21
                                                font.bold: true
                                            }

                                            MutedText {
                                                Layout.fillWidth: true
                                                text: t("section.librariesSubtitle")
                                                elide: Text.ElideRight
                                            }
                                        }
                                    }

                                    ListView {
                                        id: libraryGrid
                                        width: parent.width
                                        height: 194
                                        clip: true
                                        orientation: ListView.Horizontal
                                        boundsBehavior: Flickable.StopAtBounds
                                        spacing: 16
                                        model: appViewModel.libraries

                                        delegate: LibraryCard {
                                            width: Math.min(286, Math.max(236, libraryGrid.width * 0.26))
                                            height: 190
                                            name: model.name
                                            subtitle: model.collectionType.length > 0
                                                ? model.collectionType : model.itemType
                                            imageUrl: model.imageUrl
                                            itemCount: model.childCount
                                            onActivated: appViewModel.openLibrary(index)
                                        }

                                        WheelHandler {
                                            onWheel: function(event) {
                                                var horizontalDelta = event.angleDelta.x !== 0
                                                    ? -event.angleDelta.x
                                                    : ((event.modifiers & Qt.ShiftModifier)
                                                        ? -event.angleDelta.y : 0)
                                                if (horizontalDelta !== 0) {
                                                    libraryGrid.contentX = Math.max(0,
                                                        Math.min(libraryGrid.contentWidth - libraryGrid.width,
                                                            libraryGrid.contentX + horizontalDelta))
                                                    event.accepted = true
                                                } else if (event.angleDelta.y !== 0) {
                                                    var maxContentY = Math.max(0,
                                                        homeFlick.contentHeight - homeFlick.height)
                                                    homeFlick.contentY = Math.max(0,
                                                        Math.min(maxContentY,
                                                            homeFlick.contentY - event.angleDelta.y))
                                                    event.accepted = true
                                                } else {
                                                    event.accepted = false
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    TraditionalMediaHome {
                        anchors.fill: parent
                        visible: !homePage.trendyLayout
                        enabled: visible
                    }

                    PageLoadingPanel {
                        anchors.centerIn: parent
                        visible: homePage.trendyLayout && homePage.showInitialLoading
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

                PlayerPage { id: playerPageInstance }

                IptvPage {}

                WebDavPage {}

                TransfersPage {}

                HistoryPage {}

                ScheduledTasksPage {}

                LocalMediaPage {}

                LinkPlaybackPage {}

                GlobalHistoryPage {}

                M3u8sManagerPage {}

                SettingsPage {}
            }

            WebDavAudioMiniPlayer {
                id: webDavAudioMiniPlayer
                playerPage: playerPageInstance
                visible: appViewModel.webDavAudioPlaybackActive && appViewModel.currentView !== "player"
                z: 200
            }
        }
    }

    DropArea {
        id: localVideoDropArea
        anchors.fill: parent
        z: 10000
        enabled: root.dragFromRow < 0

        onEntered: function(drag) {
            drag.accepted = drag.hasUrls && drag.urls.length > 0
        }

        onDropped: function(drop) {
            if (!drop.hasUrls || drop.urls.length === 0) {
                drop.accepted = false
                return
            }
            if (appViewModel.openDroppedLocalVideo(drop.urls[0], playerPageInstance.playbackPosition)) {
                drop.acceptProposedAction()
            } else {
                drop.accepted = false
            }
        }

        Rectangle {
            anchors.fill: parent
            visible: localVideoDropArea.containsDrag
            color: root.withAlpha(theme.bg, 0.9)
            border.color: theme.primary
            border.width: 3

            Column {
                anchors.centerIn: parent
                spacing: 10

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: t("local.dropVideo")
                    color: theme.text
                    font.pixelSize: 30
                    font.bold: true
                }

                MutedText {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: t("local.dropVideoHint")
                    font.pixelSize: 16
                }
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

    component HeroToolbarButton: Button {
        id: heroToolbarButton
        property string iconText: ""

        implicitHeight: 46
        leftPadding: 14
        rightPadding: 16
        hoverEnabled: true
        opacity: enabled ? 1 : 0.55

        contentItem: RowLayout {
            spacing: 8

            Label {
                text: heroToolbarButton.iconText
                color: "#ffffff"
                font.pixelSize: 16
                font.bold: true
                Layout.alignment: Qt.AlignVCenter
            }

            Label {
                text: heroToolbarButton.text
                color: "#ffffff"
                font.pixelSize: 14
                font.bold: true
                elide: Text.ElideRight
                Layout.alignment: Qt.AlignVCenter
            }
        }

        background: Rectangle {
            radius: 8
            color: heroToolbarButton.down ? "#5cffffff"
                : heroToolbarButton.hovered ? "#4dffffff" : "#2effffff"
            border.color: "#55ffffff"
        }
    }

    component AudioControlIcon: Canvas {
        property string kind: "play"
        property color iconColor: theme.text
        antialiasing: true

        onKindChanged: requestPaint()
        onIconColorChanged: requestPaint()
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()

        onPaint: {
            var ctx = getContext("2d")
            var w = width
            var h = height
            ctx.clearRect(0, 0, w, h)
            ctx.fillStyle = iconColor
            ctx.strokeStyle = iconColor
            ctx.lineCap = "round"
            ctx.lineJoin = "round"

            if (kind === "play") {
                ctx.beginPath()
                ctx.moveTo(w * 0.38, h * 0.27)
                ctx.lineTo(w * 0.72, h * 0.50)
                ctx.lineTo(w * 0.38, h * 0.73)
                ctx.closePath()
                ctx.fill()
                return
            }
            if (kind === "pause") {
                ctx.fillRect(w * 0.31, h * 0.27, w * 0.13, h * 0.46)
                ctx.fillRect(w * 0.56, h * 0.27, w * 0.13, h * 0.46)
                return
            }
            if (kind === "previous" || kind === "next") {
                var previous = kind === "previous"
                ctx.fillRect(previous ? w * 0.25 : w * 0.67, h * 0.29, w * 0.09, h * 0.42)
                ctx.beginPath()
                if (previous) {
                    ctx.moveTo(w * 0.67, h * 0.27)
                    ctx.lineTo(w * 0.36, h * 0.50)
                    ctx.lineTo(w * 0.67, h * 0.73)
                } else {
                    ctx.moveTo(w * 0.33, h * 0.27)
                    ctx.lineTo(w * 0.64, h * 0.50)
                    ctx.lineTo(w * 0.33, h * 0.73)
                }
                ctx.closePath()
                ctx.fill()
                return
            }

            ctx.lineWidth = Math.max(1.7, Math.min(w, h) * 0.075)
            if (kind === "order") {
                ctx.beginPath()
                ctx.moveTo(w * 0.22, h * 0.50)
                ctx.lineTo(w * 0.72, h * 0.50)
                ctx.stroke()
                ctx.beginPath()
                ctx.moveTo(w * 0.60, h * 0.34)
                ctx.lineTo(w * 0.74, h * 0.50)
                ctx.lineTo(w * 0.60, h * 0.66)
                ctx.stroke()
                return
            }

            ctx.beginPath()
            ctx.moveTo(w * 0.24, h * 0.34)
            ctx.lineTo(w * 0.72, h * 0.34)
            ctx.lineTo(w * 0.62, h * 0.23)
            ctx.moveTo(w * 0.72, h * 0.34)
            ctx.lineTo(w * 0.62, h * 0.45)
            ctx.moveTo(w * 0.76, h * 0.66)
            ctx.lineTo(w * 0.28, h * 0.66)
            ctx.lineTo(w * 0.38, h * 0.55)
            ctx.moveTo(w * 0.28, h * 0.66)
            ctx.lineTo(w * 0.38, h * 0.77)
            ctx.stroke()
            if (kind === "repeatOne") {
                ctx.font = "600 " + Math.round(h * 0.30) + "px sans-serif"
                ctx.textAlign = "center"
                ctx.textBaseline = "middle"
                ctx.fillText("1", w * 0.50, h * 0.51)
            }
        }
    }

    component RoundedCoverImage: Canvas {
        id: roundedCoverImage
        property url source
        property real cornerRadius: 0
        property url loadedSource
        property bool imageReady: false
        readonly property int status: imageReady
            ? Image.Ready
            : coverProbe.status === Image.Error ? Image.Error
            : source.toString().length === 0 ? Image.Null : Image.Loading

        renderTarget: Canvas.Image

        function reloadSource() {
            if (loadedSource.toString().length > 0) {
                unloadImage(loadedSource)
            }
            imageReady = false
            loadedSource = source
            requestPaint()
            if (loadedSource.toString().length > 0) {
                loadImage(loadedSource)
            }
        }

        onSourceChanged: reloadSource()
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onCornerRadiusChanged: requestPaint()
        onImageLoaded: {
            imageReady = loadedSource.toString().length > 0 && isImageLoaded(loadedSource)
            requestPaint()
        }

        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            ctx.clearRect(0, 0, width, height)
            if (!imageReady || width <= 0 || height <= 0) {
                return
            }

            var sourceWidth = Math.max(1, coverProbe.implicitWidth)
            var sourceHeight = Math.max(1, coverProbe.implicitHeight)
            var targetRatio = width / height
            var sourceRatio = sourceWidth / sourceHeight
            var cropX = 0
            var cropY = 0
            var cropWidth = sourceWidth
            var cropHeight = sourceHeight
            if (sourceRatio > targetRatio) {
                cropWidth = sourceHeight * targetRatio
                cropX = (sourceWidth - cropWidth) / 2
            } else if (sourceRatio < targetRatio) {
                cropHeight = sourceWidth / targetRatio
                cropY = (sourceHeight - cropHeight) / 2
            }

            var radius = Math.max(0, Math.min(cornerRadius, width / 2, height / 2))
            ctx.beginPath()
            ctx.moveTo(radius, 0)
            ctx.lineTo(width - radius, 0)
            ctx.quadraticCurveTo(width, 0, width, radius)
            ctx.lineTo(width, height - radius)
            ctx.quadraticCurveTo(width, height, width - radius, height)
            ctx.lineTo(radius, height)
            ctx.quadraticCurveTo(0, height, 0, height - radius)
            ctx.lineTo(0, radius)
            ctx.quadraticCurveTo(0, 0, radius, 0)
            ctx.closePath()
            ctx.clip()
            ctx.drawImage(loadedSource,
                          cropX, cropY, cropWidth, cropHeight,
                          0, 0, width, height)
        }

        Image {
            id: coverProbe
            source: roundedCoverImage.source
            asynchronous: true
            cache: false
            visible: false
            onStatusChanged: {
                roundedCoverImage.requestPaint()
            }
        }
    }

    component AudioTransportButton: Button {
        id: transportButton
        property string iconKind: "play"
        property bool primaryAction: false
        property bool loading: false
        implicitWidth: primaryAction ? 54 : 44
        implicitHeight: primaryAction ? 54 : 44
        padding: 0

        contentItem: Item {
            AudioControlIcon {
                anchors.fill: parent
                visible: !transportButton.loading
                kind: transportButton.iconKind
                iconColor: transportButton.enabled
                    ? (transportButton.primaryAction ? "#ffffff" : theme.text)
                    : theme.subtle
            }

            ThumbnailLoadingIcon {
                anchors.centerIn: parent
                running: transportButton.loading
                iconSize: Math.round(Math.min(parent.width, parent.height) * 0.66)
                accentColor: transportButton.primaryAction ? "#ffffff" : theme.primary
                backgroundVisible: false
            }
        }
        background: Rectangle {
            radius: width / 2
            color: transportButton.primaryAction
                ? (transportButton.hovered ? theme.primaryHover : theme.primary)
                : transportButton.down ? root.withAlpha(theme.primary, 0.18)
                : transportButton.hovered ? theme.elevatedHover : theme.elevated
            border.width: transportButton.activeFocus ? 2 : 1
            border.color: transportButton.primaryAction
                ? theme.primary
                : transportButton.activeFocus || transportButton.hovered
                    ? root.withAlpha(theme.primary, 0.72) : theme.border
        }
    }

    component PlayerTransportButton: Button {
        id: playerTransportButton
        property string iconKind: "play"
        property string badgeText: ""
        property bool primaryAction: false

        implicitWidth: primaryAction ? 42 : 36
        implicitHeight: primaryAction ? 42 : 36
        padding: 0
        hoverEnabled: true
        opacity: enabled ? 1 : 0.42
        scale: down ? 0.94 : 1

        Behavior on scale {
            NumberAnimation { duration: 80; easing.type: Easing.OutCubic }
        }

        contentItem: Item {
            AudioControlIcon {
                anchors.centerIn: parent
                width: playerTransportButton.primaryAction ? 32 : 28
                height: width
                kind: playerTransportButton.iconKind
                iconColor: playerTransportButton.primaryAction ? "#ffffff" : "#e8edf4"
            }

            Label {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.rightMargin: 1
                anchors.bottomMargin: 1
                visible: playerTransportButton.badgeText.length > 0
                text: playerTransportButton.badgeText
                color: "#c7d0dd"
                font.pixelSize: 8
                font.bold: true
            }
        }

        background: Rectangle {
            radius: width / 2
            color: playerTransportButton.primaryAction
                ? (playerTransportButton.hovered ? "#6aa0ff" : "#4f8cff")
                : playerTransportButton.down ? "#38ffffff"
                : playerTransportButton.hovered ? "#2cffffff" : "#16ffffff"
            border.width: playerTransportButton.activeFocus ? 2 : 1
            border.color: playerTransportButton.primaryAction
                ? "#90b8ff"
                : playerTransportButton.activeFocus || playerTransportButton.hovered
                    ? "#7da7d8" : "#35ffffff"
        }

        ToolTip.visible: hovered
        ToolTip.text: Accessible.name
    }

    component PlayerChromeButton: Button {
        id: playerChromeButton
        property string iconKind: ""
        property string iconText: ""
        property bool danger: false
        property bool compact: false

        implicitWidth: compact ? 34 : Math.max(64, chromeButtonContent.implicitWidth + 20)
        implicitHeight: compact ? 30 : 34
        leftPadding: compact ? 0 : 10
        rightPadding: compact ? 0 : 10
        hoverEnabled: true
        opacity: enabled ? 1 : 0.42
        scale: down ? 0.97 : 1

        Behavior on scale {
            NumberAnimation { duration: 80; easing.type: Easing.OutCubic }
        }

        contentItem: RowLayout {
            id: chromeButtonContent
            spacing: playerChromeButton.compact ? 0 : 6

            Item {
                Layout.preferredWidth: 20
                Layout.preferredHeight: 20
                Layout.alignment: Qt.AlignVCenter

                AudioControlIcon {
                    anchors.fill: parent
                    visible: playerChromeButton.iconKind.length > 0
                    kind: playerChromeButton.iconKind
                    iconColor: playerChromeButton.danger ? "#ffdce3" : "#e8edf4"
                }

                Label {
                    anchors.centerIn: parent
                    visible: playerChromeButton.iconKind.length === 0
                    text: playerChromeButton.iconText
                    color: playerChromeButton.danger ? "#ffdce3" : "#e8edf4"
                    font.pixelSize: playerChromeButton.iconText.length > 2 ? 9 : 12
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }

            Label {
                visible: !playerChromeButton.compact && playerChromeButton.text.length > 0
                text: playerChromeButton.text
                color: playerChromeButton.danger ? "#ffe8ec" : "#edf2f8"
                font.pixelSize: 12
                font.bold: true
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
            }
        }

        background: Rectangle {
            radius: 7
            color: playerChromeButton.danger
                ? playerChromeButton.down ? "#a13b4c"
                    : playerChromeButton.hovered ? "#7d3040" : "#47242d"
                : playerChromeButton.down ? "#3b79d8"
                    : playerChromeButton.hovered ? "#354657" : "#18ffffff"
            border.width: playerChromeButton.activeFocus ? 2 : 1
            border.color: playerChromeButton.danger
                ? playerChromeButton.hovered ? "#e27183" : "#8f4856"
                : playerChromeButton.activeFocus || playerChromeButton.hovered
                    ? "#6f9ed4" : "#34ffffff"
        }

        ToolTip.visible: hovered
        ToolTip.text: Accessible.name
    }

    component AudioRepeatButton: Button {
        id: repeatButton
        property string iconKind: "order"
        property bool selected: false
        implicitWidth: 34
        implicitHeight: 32
        padding: 0

        contentItem: AudioControlIcon {
            kind: repeatButton.iconKind
            iconColor: repeatButton.selected ? "#ffffff" : theme.muted
        }
        background: Rectangle {
            radius: 5
            color: repeatButton.selected
                ? theme.primary
                : repeatButton.hovered ? theme.elevatedHover : "transparent"
            border.width: repeatButton.activeFocus ? 2 : 1
            border.color: repeatButton.selected
                ? theme.primary
                : repeatButton.activeFocus ? root.withAlpha(theme.primary, 0.72) : "transparent"
        }
    }

    component AudioRepeatModeSwitch: Rectangle {
        implicitWidth: 112
        implicitHeight: 38
        radius: 8
        color: theme.input
        border.color: theme.border

        RowLayout {
            anchors.fill: parent
            anchors.margins: 3
            spacing: 2

            AudioRepeatButton {
                Layout.fillWidth: true
                Layout.fillHeight: true
                iconKind: "order"
                selected: appViewModel.webDavAudioRepeatMode === "off"
                Accessible.name: t("webdav.repeatOff")
                ToolTip.visible: hovered
                ToolTip.text: t("webdav.repeatOff")
                onClicked: appViewModel.webDavAudioRepeatMode = "off"
            }

            AudioRepeatButton {
                Layout.fillWidth: true
                Layout.fillHeight: true
                iconKind: "repeatOne"
                selected: appViewModel.webDavAudioRepeatMode === "one"
                Accessible.name: t("webdav.repeatOne")
                ToolTip.visible: hovered
                ToolTip.text: t("webdav.repeatOne")
                onClicked: appViewModel.webDavAudioRepeatMode = "one"
            }

            AudioRepeatButton {
                Layout.fillWidth: true
                Layout.fillHeight: true
                iconKind: "repeatAll"
                selected: appViewModel.webDavAudioRepeatMode === "all"
                Accessible.name: t("webdav.repeatAll")
                ToolTip.visible: hovered
                ToolTip.text: t("webdav.repeatAll")
                onClicked: appViewModel.webDavAudioRepeatMode = "all"
            }
        }
    }

    component WebDavAudioMiniPlayer: Rectangle {
        id: miniPlayer
        property var playerPage: null
        property bool placementInitialized: false
        property bool manuallyPositioned: false
        readonly property real safeMargin: 22

        width: Math.min(400, Math.max(340, parent ? parent.width * 0.34 : 360))
        height: 94
        radius: 12
        color: darkTheme ? "#f21d232b" : "#f7ffffff"
        border.color: root.withAlpha(theme.primary, darkTheme ? 0.54 : 0.40)
        border.width: 1
        clip: true

        function clampPosition() {
            if (!parent || !placementInitialized) {
                return
            }
            x = Math.max(safeMargin, Math.min(x, parent.width - width - safeMargin))
            y = Math.max(safeMargin, Math.min(y, parent.height - height - safeMargin))
        }

        function ensurePlacement() {
            if (!visible || !parent) {
                return
            }
            if (!placementInitialized || !manuallyPositioned) {
                x = Math.max(safeMargin, parent.width - width - safeMargin)
                y = Math.max(safeMargin, parent.height - height - safeMargin)
                placementInitialized = true
                return
            }
            clampPosition()
        }

        onVisibleChanged: Qt.callLater(ensurePlacement)
        onWidthChanged: Qt.callLater(ensurePlacement)
        onHeightChanged: Qt.callLater(ensurePlacement)
        Component.onCompleted: Qt.callLater(ensurePlacement)

        Connections {
            target: miniPlayer.parent
            function onWidthChanged() { miniPlayer.ensurePlacement() }
            function onHeightChanged() { miniPlayer.ensurePlacement() }
        }

        DragHandler {
            id: miniPlayerDrag
            target: miniPlayer
            xAxis.minimum: miniPlayer.safeMargin
            xAxis.maximum: Math.max(miniPlayer.safeMargin,
                                    miniPlayer.parent ? miniPlayer.parent.width - miniPlayer.width - miniPlayer.safeMargin : miniPlayer.safeMargin)
            yAxis.minimum: miniPlayer.safeMargin
            yAxis.maximum: Math.max(miniPlayer.safeMargin,
                                    miniPlayer.parent ? miniPlayer.parent.height - miniPlayer.height - miniPlayer.safeMargin : miniPlayer.safeMargin)
            cursorShape: active ? Qt.ClosedHandCursor : Qt.OpenHandCursor
            onActiveChanged: {
                if (!active) {
                    miniPlayer.manuallyPositioned = true
                    miniPlayer.clampPosition()
                }
            }
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 10
            anchors.topMargin: 11
            anchors.bottomMargin: 12
            spacing: 11

            Rectangle {
                id: miniPlayerArtwork
                readonly property real artworkSize: 58

                Layout.minimumWidth: artworkSize
                Layout.preferredWidth: artworkSize
                Layout.maximumWidth: artworkSize
                Layout.minimumHeight: artworkSize
                Layout.preferredHeight: artworkSize
                Layout.maximumHeight: artworkSize
                radius: 9
                color: root.withAlpha(theme.primary, darkTheme ? 0.24 : 0.12)
                border.color: root.withAlpha(theme.primary, 0.52)
                clip: true

                RoundedCoverImage {
                    id: miniPlayerCoverImage
                    anchors.fill: parent
                    anchors.margins: 1
                    source: miniPlayer.playerPage ? miniPlayer.playerPage.audioCoverUrl : ""
                    cornerRadius: 8
                    visible: status === Image.Ready
                }

                Label {
                    anchors.centerIn: parent
                    text: "\u266B"
                    color: theme.primary
                    font.pixelSize: 29
                    font.bold: true
                    visible: miniPlayerCoverImage.status !== Image.Ready
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 3

                    Label {
                        Layout.fillWidth: true
                        text: miniPlayer.playerPage
                            ? miniPlayer.playerPage.audioDisplayTitle
                            : appViewModel.webDavAudioCurrentName
                        color: theme.text
                        font.pixelSize: 14
                        font.bold: true
                        elide: Text.ElideMiddle
                    }

                    MutedText {
                        Layout.fillWidth: true
                        text: (miniPlayer.playerPage && miniPlayer.playerPage.audioDisplayArtist.length > 0
                                   ? miniPlayer.playerPage.audioDisplayArtist
                                   : appViewModel.currentServerName)
                            + "  \u00B7  "
                            + (miniPlayer.playerPage ? miniPlayer.playerPage.formatTime(miniPlayer.playerPage.audioPosition) : "00:00")
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }
                }

                TapHandler {
                    onTapped: appViewModel.restoreWebDavAudioPlayer()
                }

                ToolTip.visible: miniPlayerCentralHover.hovered
                ToolTip.text: t("webdav.openAudioPlayer")

                HoverHandler { id: miniPlayerCentralHover }
            }

            AudioTransportButton {
                primaryAction: true
                loading: miniPlayer.playerPage ? miniPlayer.playerPage.audioPlaybackLoading : false
                iconKind: miniPlayer.playerPage && miniPlayer.playerPage.audioPaused ? "play" : "pause"
                Accessible.name: loading ? t("player.loading")
                    : iconKind === "play" ? t("action.resume") : t("action.pause")
                ToolTip.visible: hovered
                ToolTip.text: Accessible.name
                onClicked: {
                    if (!loading && miniPlayer.playerPage) {
                        miniPlayer.playerPage.toggleAudioPause()
                    }
                }
            }

            Button {
                id: miniPlayerExitButton
                implicitWidth: 32
                implicitHeight: 32
                padding: 0
                Accessible.name: t("action.exitPlayback")
                ToolTip.visible: hovered
                ToolTip.text: Accessible.name
                onClicked: if (miniPlayer.playerPage) miniPlayer.playerPage.stopAudioPlayback()

                contentItem: Label {
                    text: "\u00D7"
                    color: miniPlayerExitButton.hovered ? "#ffffff" : theme.muted
                    font.pixelSize: 22
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                background: Rectangle {
                    radius: width / 2
                    color: miniPlayerExitButton.hovered ? theme.danger : "transparent"
                    border.color: miniPlayerExitButton.hovered ? theme.danger : theme.border
                }
            }
        }

        Rectangle {
            id: miniPlayerProgressTrack
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            anchors.bottomMargin: 5
            height: 4
            radius: height / 2
            color: root.withAlpha(theme.muted, darkTheme ? 0.30 : 0.18)
            clip: true

            Rectangle {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: parent.width * Math.max(0, Math.min(1,
                    miniPlayer.playerPage && miniPlayer.playerPage.audioDuration > 0
                        ? miniPlayer.playerPage.audioPosition / miniPlayer.playerPage.audioDuration : 0))
                radius: parent.radius
                color: theme.primary
                visible: width > 0
            }
        }
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

        function focusInput() {
            serverSearchInput.forceActiveFocus()
        }

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
        property bool backgroundVisible: true

        width: iconSize
        height: iconSize
        visible: running
        opacity: running ? 1 : 0

        Behavior on opacity { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }

        Rectangle {
            anchors.fill: parent
            visible: loadingIcon.backgroundVisible
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
        id: posterFrame
        property string imageUrl: ""
        property string fallbackText: "?"
        radius: 8
        color: posterImage.status === Image.Ready ? "transparent" : theme.input
        border.color: theme.border
        border.width: 0
        clip: true

        Image {
            id: posterImage
            anchors.fill: parent
            source: imageUrl
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            visible: false
        }

        Rectangle {
            id: posterMask
            anchors.fill: parent
            radius: posterFrame.radius
            color: "#ffffff"
            visible: false
            layer.enabled: true
        }

        MultiEffect {
            anchors.fill: parent
            source: posterImage
            autoPaddingEnabled: false
            maskEnabled: true
            maskSource: posterMask
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
                    } else if (serviceIcon.normalizedType === "link") {
                        context.save()
                        context.translate(17, 17)
                        context.rotate(-Math.PI / 4)
                        context.strokeStyle = "#ffffff"
                        context.lineWidth = 2.6
                        roundedRectPath(context, -13, -5, 16, 10, 5)
                        context.stroke()
                        roundedRectPath(context, -3, -5, 16, 10, 5)
                        context.stroke()
                        context.restore()
                    } else if (serviceIcon.normalizedType === "history"
                               || serviceIcon.normalizedType === "globalhistory") {
                        context.strokeStyle = "#ffffff"
                        context.fillStyle = "#ffffff"
                        context.lineWidth = 2.4
                        context.beginPath()
                        context.arc(17, 17, 10.5, -Math.PI * 0.15, Math.PI * 1.55)
                        context.stroke()
                        context.beginPath()
                        context.moveTo(6.2, 10.2)
                        context.lineTo(5.4, 17.1)
                        context.lineTo(11.8, 14.6)
                        context.closePath()
                        context.fill()
                        context.beginPath()
                        context.moveTo(17, 10.5)
                        context.lineTo(17, 17)
                        context.lineTo(22.2, 20.1)
                        context.stroke()
                    } else if (serviceIcon.normalizedType === "m3u8s") {
                        context.strokeStyle = "#ffffff"
                        context.fillStyle = "#ffffff"
                        context.lineWidth = 2.1
                        roundedRectPath(context, 3.5, 7, 27, 20, 4)
                        context.stroke()
                        context.beginPath()
                        context.moveTo(8.5, 7)
                        context.lineTo(8.5, 27)
                        context.moveTo(25.5, 7)
                        context.lineTo(25.5, 27)
                        context.stroke()
                        for (var frameIndex = 0; frameIndex < 3; ++frameIndex) {
                            var frameY = 10 + frameIndex * 6.5
                            context.fillRect(5.5, frameY, 1.8, 2.5)
                            context.fillRect(26.7, frameY, 1.8, 2.5)
                        }
                        context.fillStyle = serviceIcon.accentColor
                        roundedRectPath(context, 12, 16, 10, 8, 2)
                        context.fill()
                        context.strokeStyle = "#ffffff"
                        context.lineWidth = 1.8
                        context.beginPath()
                        context.arc(17, 16, 3.2, Math.PI, 0)
                        context.stroke()
                        context.fillStyle = "#ffffff"
                        context.beginPath()
                        context.arc(17, 20, 1.2, 0, Math.PI * 2)
                        context.fill()
                    } else if (serviceIcon.normalizedType === "local") {
                        context.fillStyle = "#ffffff"
                        context.beginPath()
                        context.moveTo(4, 10)
                        context.lineTo(13, 10)
                        context.lineTo(16, 14)
                        context.lineTo(30, 14)
                        context.lineTo(30, 28)
                        context.lineTo(4, 28)
                        context.closePath()
                        context.fill()

                        context.fillStyle = serviceIcon.accentColor
                        context.beginPath()
                        context.moveTo(15, 17)
                        context.lineTo(15, 25)
                        context.lineTo(22, 21)
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
        property string leadingStatusText: autoLogin ? t("status.autoLogin") : t("status.passwordRequired")
        property color leadingStatusColor: autoLogin ? theme.success : theme.warning
        property string trailingStatusText: hasSession ? t("status.ready") : t("status.noSession")
        property color trailingStatusColor: hasSession ? theme.primary : theme.subtle
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
                    text: card.leadingStatusText
                    accentColor: card.leadingStatusColor
                }

                Item { Layout.fillWidth: true }

                ServiceStatusChip {
                    Layout.maximumWidth: serviceStatusRow.width * 0.46
                    text: card.trailingStatusText
                    accentColor: card.trailingStatusColor
                }
            }
        }
    }

    component TraditionalMediaHome: Item {
        id: traditionalHome
        property bool showInitialLoading: appViewModel.homeLoading
            && appViewModel.continueItems.count === 0
            && appViewModel.libraries.count === 0

        Flickable {
            id: traditionalHomeFlick
            anchors.fill: parent
            contentWidth: width
            contentHeight: traditionalHomeColumn.implicitHeight
            clip: true
            opacity: traditionalHome.showInitialLoading ? 0.24 : 1
            boundsBehavior: Flickable.StopAtBounds

            Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }

            ColumnLayout {
                id: traditionalHomeColumn
                width: traditionalHomeFlick.width
                spacing: 28

                SectionHeader {
                    title: t("section.continueWatching")
                    subtitle: t("section.continueSubtitle")
                }

                Item {
                    id: traditionalContinueRail
                    Layout.fillWidth: true
                    Layout.preferredHeight: appViewModel.continueItems.count > 0 ? 316 : 72

                    function maxContentX() {
                        return Math.max(0, traditionalContinueList.contentWidth
                            - traditionalContinueList.width)
                    }

                    function scrollBy(delta) {
                        traditionalContinueList.contentX = Math.max(0,
                            Math.min(maxContentX(), traditionalContinueList.contentX + delta))
                    }

                    RowLayout {
                        anchors.fill: parent
                        visible: appViewModel.continueItems.count > 0
                        spacing: 10

                        IconButton {
                            text: "‹"
                            enabled: traditionalContinueList.contentX > 1
                            onClicked: traditionalContinueRail.scrollBy(
                                -Math.max(360, traditionalContinueList.width * 0.82))
                        }

                        ListView {
                            id: traditionalContinueList
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            orientation: ListView.Horizontal
                            boundsBehavior: Flickable.StopAtBounds
                            spacing: 14
                            model: appViewModel.continueItems

                            delegate: TraditionalContinueWatchingCard {
                                width: 172
                                height: 306
                                title: model.name.length > 0 ? model.name : model.seriesName
                                metadata: model.itemType === "Episode" && model.seriesName.length > 0
                                    ? model.seriesName + (appViewModel.formatSeasonEpisode(
                                        model.parentIndexNumber, model.indexNumber).length > 0
                                        ? " · " + appViewModel.formatSeasonEpisode(
                                            model.parentIndexNumber, model.indexNumber) : "")
                                    : appViewModel.formatSeasonEpisode(
                                        model.parentIndexNumber, model.indexNumber)
                                progressText: appViewModel.formatContinueProgress(model.playedPercentage)
                                imageUrl: model.continueImageUrl
                                progress: model.playedPercentage
                                onActivated: appViewModel.openContinueItem(index)
                            }

                            WheelHandler {
                                onWheel: function(event) {
                                    var delta = event.angleDelta.y !== 0
                                        ? -event.angleDelta.y : -event.angleDelta.x
                                    if (delta !== 0) {
                                        traditionalContinueRail.scrollBy(delta)
                                        event.accepted = true
                                    }
                                }
                            }
                        }

                        IconButton {
                            text: "›"
                            enabled: traditionalContinueList.contentX
                                < traditionalContinueRail.maxContentX() - 1
                            onClicked: traditionalContinueRail.scrollBy(
                                Math.max(360, traditionalContinueList.width * 0.82))
                        }
                    }

                    MutedText {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        visible: appViewModel.continueItems.count === 0
                            && !traditionalHome.showInitialLoading
                        text: t("section.noProgress")
                    }
                }

                SectionHeader {
                    title: t("section.libraries")
                    subtitle: t("section.librariesSubtitle")
                }

                GridView {
                    id: traditionalLibraryGrid
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.max(260,
                        Math.ceil(count / Math.max(1, Math.floor(width / 214))) * 230)
                    clip: true
                    interactive: false
                    model: appViewModel.libraries
                    cellWidth: Math.max(190, width / Math.max(1, Math.floor(width / 214)))
                    cellHeight: 230

                    delegate: TraditionalLibraryCard {
                        width: traditionalLibraryGrid.cellWidth - 16
                        height: 210
                        name: model.name
                        subtitle: model.collectionType.length > 0
                            ? model.collectionType : model.itemType
                        imageUrl: model.imageUrl
                        onActivated: appViewModel.openLibrary(index)
                    }
                }
            }
        }

        PageLoadingPanel {
            anchors.centerIn: parent
            visible: traditionalHome.showInitialLoading
            title: t("loading.home")
            subtitle: t("loading.homeHint")
        }
    }

    component TraditionalLibraryCard: Rectangle {
        id: traditionalLibraryCard
        signal activated()
        property string name: ""
        property string subtitle: ""
        property string imageUrl: ""

        radius: 8
        color: traditionalLibraryMouse.containsMouse ? theme.elevatedHover : theme.elevated
        border.color: theme.border

        MouseArea {
            id: traditionalLibraryMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: traditionalLibraryCard.activated()
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 10

            PosterImage {
                Layout.fillWidth: true
                Layout.preferredHeight: 126
                imageUrl: traditionalLibraryCard.imageUrl
                fallbackText: traditionalLibraryCard.name.length > 0
                    ? traditionalLibraryCard.name[0] : "?"
            }

            Label {
                Layout.fillWidth: true
                text: traditionalLibraryCard.name
                color: theme.text
                font.pixelSize: 15
                font.bold: true
                elide: Text.ElideRight
            }

            MutedText {
                Layout.fillWidth: true
                text: traditionalLibraryCard.subtitle
                elide: Text.ElideRight
            }
        }
    }

    component TraditionalContinueWatchingCard: Rectangle {
        id: traditionalContinueCard
        signal activated()
        property string title: ""
        property string metadata: ""
        property string progressText: ""
        property string imageUrl: ""
        property real progress: 0

        radius: 8
        color: traditionalContinueMouse.containsMouse ? theme.elevatedHover : theme.elevated
        border.color: theme.border
        clip: true
        scale: traditionalContinueMouse.containsMouse ? 0.985 : 1

        Behavior on scale { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }

        MouseArea {
            id: traditionalContinueMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: traditionalContinueCard.activated()
        }

        PosterImage {
            anchors.fill: parent
            imageUrl: traditionalContinueCard.imageUrl
            fallbackText: traditionalContinueCard.title.length > 0
                ? traditionalContinueCard.title[0] : "?"
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: Math.min(parent.height * 0.54, 164)
            gradient: Gradient {
                GradientStop { position: 0; color: "#00000000" }
                GradientStop { position: 0.34; color: "#a3000000" }
                GradientStop { position: 1; color: "#e8000000" }
            }
        }

        ColumnLayout {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 12
            spacing: 4

            Label {
                Layout.fillWidth: true
                text: traditionalContinueCard.title
                color: "#ffffff"
                font.pixelSize: 14
                font.bold: true
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                elide: Text.ElideRight
                style: Text.Raised
                styleColor: "#cc000000"
            }

            Label {
                Layout.fillWidth: true
                visible: traditionalContinueCard.metadata.length > 0
                text: traditionalContinueCard.metadata
                color: "#d8e1ee"
                font.pixelSize: 12
                elide: Text.ElideRight
                style: Text.Raised
                styleColor: "#bb000000"
            }

            Label {
                Layout.fillWidth: true
                text: traditionalContinueCard.progressText
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
                    width: parent.width * Math.min(100, traditionalContinueCard.progress) / 100
                    color: theme.primary
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
        property int itemCount: 0

        radius: 8
        color: "transparent"
        border.width: 0
        scale: mouse.containsMouse ? 0.985 : 1

        Behavior on scale { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }

        MouseArea {
            id: mouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: activated()
        }

        PosterImage {
            id: libraryCover
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 140
            radius: 12
            imageUrl: libraryCard.imageUrl
            fallbackText: libraryCard.name.length > 0 ? libraryCard.name[0] : "?"
            border.color: mouse.containsMouse ? theme.primary : theme.border
        }

        Rectangle {
            anchors.right: libraryCover.right
            anchors.top: libraryCover.top
            anchors.margins: 9
            visible: libraryCard.itemCount > 0
            width: itemCountLabel.implicitWidth + 14
            height: 25
            radius: 8
            color: "#b30b0e13"
            border.color: "#4dffffff"

            Label {
                id: itemCountLabel
                anchors.centerIn: parent
                text: libraryCard.itemCount
                color: "#ffffff"
                font.pixelSize: 11
                font.bold: true
            }
        }

        Label {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: libraryCover.bottom
            anchors.topMargin: 9
            text: libraryCard.name
            color: theme.text
            font.pixelSize: 15
            font.bold: true
            elide: Text.ElideRight
        }

        MutedText {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            text: libraryCard.subtitle
            elide: Text.ElideRight
        }
    }

    component MediaPoster: Rectangle {
        id: mediaPoster
        signal activated()
        property string title: ""
        property string subtitle: ""
        property string imageUrl: ""
        property real progress: 0

        radius: 8
        color: "transparent"
        border.width: 0
        clip: false

        MouseArea {
            id: posterMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: activated()
        }

        PosterImage {
            id: mediaPosterImage
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: Math.max(0, parent.height - 52)
            radius: 8
            imageUrl: mediaPoster.imageUrl
            fallbackText: mediaPoster.title.length > 0 ? mediaPoster.title[0] : "?"
            border.color: posterMouse.containsMouse ? theme.primary : theme.border
            scale: posterMouse.containsMouse ? 0.985 : 1

            Behavior on scale { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
        }

        Rectangle {
            anchors.left: mediaPosterImage.left
            anchors.right: mediaPosterImage.right
            anchors.bottom: mediaPosterImage.bottom
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            anchors.bottomMargin: 6
            height: 5
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

        Label {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: mediaPosterImage.bottom
            anchors.topMargin: 7
            text: mediaPoster.title
            color: theme.text
            font.pixelSize: 14
            font.bold: true
            elide: Text.ElideRight
        }

        Label {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            text: mediaPoster.subtitle
            color: theme.muted
            font.pixelSize: 12
            elide: Text.ElideRight
        }
    }

    component ContinueWatchingCard: Rectangle {
        id: continueCard
        signal activated()
        property string title: ""
        property string seriesName: ""
        property string seasonEpisode: ""
        property string progressText: ""
        property string imageUrl: ""
        property string backdropUrl: ""
        property real progress: 0

        radius: 8
        color: "transparent"
        border.width: 0
        clip: false
        scale: cardMouse.containsMouse ? 0.985 : 1.0

        Behavior on scale { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }

        MouseArea {
            id: cardMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: activated()
        }

        PosterImage {
            id: continueImage
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: Math.max(0, parent.height - 48)
            radius: 12
            imageUrl: continueCard.backdropUrl.length > 0
                ? continueCard.backdropUrl : continueCard.imageUrl
            fallbackText: continueCard.title.length > 0 ? continueCard.title[0] : "?"
            border.color: cardMouse.containsMouse ? theme.primary : theme.border
        }

        Rectangle {
            anchors.left: continueImage.left
            anchors.right: continueImage.right
            anchors.bottom: continueImage.bottom
            height: 48
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#00000000" }
                GradientStop { position: 0.28; color: "#73000000" }
                GradientStop { position: 1.0; color: "#e8000000" }
            }
        }

        Label {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: continueImage.bottom
            anchors.topMargin: 7
            text: continueCard.title
            color: theme.text
            font.pixelSize: 14
            font.bold: true
            elide: Text.ElideRight
        }

        Label {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            text: continueCard.seriesName.length > 0
                ? continueCard.seriesName + (continueCard.seasonEpisode.length > 0
                    ? " · " + continueCard.seasonEpisode : "")
                : (continueCard.seasonEpisode.length > 0
                    ? continueCard.seasonEpisode : continueCard.progressText)
            color: theme.muted
            font.pixelSize: 12
            elide: Text.ElideRight
        }

        Label {
            anchors.right: continueImage.right
            anchors.bottom: continueImage.bottom
            anchors.rightMargin: 9
            anchors.bottomMargin: 10
            text: continueCard.progressText
            color: "#ffffff"
            font.pixelSize: 11
            font.bold: true
            style: Text.Raised
            styleColor: "#cc000000"
        }

        Rectangle {
            anchors.left: continueImage.left
            anchors.right: continueImage.right
            anchors.bottom: continueImage.bottom
            anchors.leftMargin: 7
            anchors.rightMargin: 7
            anchors.bottomMargin: 5
            height: 4
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
        signal exportTsslRequested()
        property string title: ""
        property string subtitle: ""
        property bool directory: false
        property bool playable: false
        property bool encryptedHls: false

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
                visible: fileRow.encryptedHls
                text: t("webdav.tsslExport")
                onClicked: fileRow.exportTsslRequested()
            }

            ModernButton {
                text: t("action.download")
                onClicked: fileRow.downloadRequested()
            }
        }
    }

    component WebDavDisplayModeSwitch: Rectangle {
        implicitWidth: 318
        implicitHeight: 38
        radius: 8
        color: theme.input
        border.color: theme.border

        RowLayout {
            anchors.fill: parent
            anchors.margins: 3
            spacing: 3

            TransferFilterButton {
                Layout.fillWidth: true
                Layout.fillHeight: true
                selected: appViewModel.webDavDisplayMode === "default"
                text: "\u25a4  " + t("webdav.modeDefault")
                onClicked: appViewModel.webDavDisplayMode = "default"
            }

            TransferFilterButton {
                Layout.fillWidth: true
                Layout.fillHeight: true
                selected: appViewModel.webDavDisplayMode === "video"
                text: "\u25b6  " + t("webdav.modeVideo")
                onClicked: appViewModel.webDavDisplayMode = "video"
            }

            TransferFilterButton {
                Layout.fillWidth: true
                Layout.fillHeight: true
                selected: appViewModel.webDavDisplayMode === "audio"
                text: "\u266B  " + t("webdav.modeAudio")
                onClicked: appViewModel.webDavDisplayMode = "audio"
            }
        }
    }

    component WebDavMediaCard: Rectangle {
        id: mediaCard
        signal activated()
        signal downloadRequested()
        signal exportTsslRequested()
        property string title: ""
        property string contentType: ""
        property real bytes: -1
        property bool directory: false
        property bool encryptedHls: false
        readonly property color accentColor: directory ? theme.primary : theme.success

        function badgeText() {
            if (directory) {
                return t("webdav.folder")
            }
            var dot = title.lastIndexOf(".")
            if (dot >= 0 && dot < title.length - 1) {
                return title.substring(dot + 1).toUpperCase()
            }
            return t("webdav.video")
        }

        function detailText() {
            if (directory) {
                return t("webdav.folder")
            }
            if (bytes >= 0) {
                return root.formatBytes(bytes)
            }
            return contentType.length > 0 ? contentType : t("webdav.video")
        }

        radius: 12
        color: mediaMouse.containsMouse ? theme.elevatedHover : theme.elevated
        border.color: mediaMouse.containsMouse ? root.withAlpha(accentColor, 0.78) : theme.border
        border.width: 1
        clip: true
        scale: mediaMouse.containsMouse ? 1.012 : 1.0
        implicitHeight: 214

        Behavior on color { ColorAnimation { duration: 140; easing.type: Easing.OutCubic } }
        Behavior on border.color { ColorAnimation { duration: 140; easing.type: Easing.OutCubic } }
        Behavior on scale { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }

        MouseArea {
            id: mediaMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: mediaCard.activated()
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 8
            spacing: 7

            Rectangle {
                id: davThumbnail
                Layout.fillWidth: true
                Layout.preferredHeight: 120
                radius: 9
                clip: true
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop {
                        position: 0.0
                        color: root.withAlpha(mediaCard.accentColor, darkTheme ? 0.34 : 0.22)
                    }
                    GradientStop {
                        position: 1.0
                        color: root.withAlpha(mediaCard.accentColor, darkTheme ? 0.10 : 0.06)
                    }
                }

                Rectangle {
                    width: 112
                    height: 112
                    radius: 56
                    anchors.right: parent.right
                    anchors.rightMargin: -26
                    anchors.verticalCenter: parent.verticalCenter
                    color: root.withAlpha(mediaCard.accentColor, darkTheme ? 0.12 : 0.08)
                }

                Rectangle {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.margins: 9
                    height: 24
                    width: Math.min(parent.width - 18, davBadge.implicitWidth + 18)
                    radius: 7
                    color: root.withAlpha(mediaCard.accentColor, darkTheme ? 0.26 : 0.16)
                    border.color: root.withAlpha(mediaCard.accentColor, 0.42)

                    Label {
                        id: davBadge
                        anchors.centerIn: parent
                        text: mediaCard.badgeText()
                        color: mediaCard.accentColor
                        font.pixelSize: 10
                        font.bold: true
                        font.capitalization: Font.AllUppercase
                        elide: Text.ElideRight
                    }
                }

                Item {
                    anchors.centerIn: parent
                    width: 82
                    height: 62
                    visible: mediaCard.directory

                    Rectangle {
                        x: 8
                        y: 8
                        width: 34
                        height: 18
                        radius: 6
                        color: root.withAlpha(mediaCard.accentColor, 0.82)
                    }

                    Rectangle {
                        x: 5
                        y: 18
                        width: 72
                        height: 40
                        radius: 9
                        color: mediaCard.accentColor
                        border.color: Qt.rgba(1, 1, 1, 0.34)
                    }
                }

                Rectangle {
                    anchors.centerIn: parent
                    width: 82
                    height: 54
                    radius: 11
                    visible: !mediaCard.directory
                    color: root.withAlpha(mediaCard.accentColor, darkTheme ? 0.24 : 0.16)
                    border.color: root.withAlpha(mediaCard.accentColor, 0.72)
                    border.width: 2

                    Label {
                        anchors.centerIn: parent
                        anchors.horizontalCenterOffset: 2
                        text: "\u25b6"
                        color: mediaCard.accentColor
                        font.pixelSize: 28
                        font.bold: true
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                Layout.preferredHeight: 36
                text: mediaCard.title
                color: theme.text
                font.pixelSize: 14
                font.bold: true
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                spacing: 6

                MutedText {
                    Layout.fillWidth: true
                    text: mediaCard.detailText()
                    elide: Text.ElideRight
                }

                IconButton {
                    visible: mediaCard.encryptedHls
                    implicitWidth: 30
                    implicitHeight: 28
                    text: "K"
                    ToolTip.visible: hovered
                    ToolTip.text: t("webdav.tsslExport")
                    onClicked: mediaCard.exportTsslRequested()
                }

                IconButton {
                    implicitWidth: 30
                    implicitHeight: 28
                    text: "\u2193"
                    ToolTip.visible: hovered
                    ToolTip.text: t("action.download")
                    onClicked: mediaCard.downloadRequested()
                }
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

    component DetailOverlayButton: Button {
        id: overlayButton
        readonly property color foregroundColor: darkTheme ? "#17191d" : theme.text
        readonly property color surfaceColor: darkTheme ? "#f3eee4" : theme.surface
        implicitWidth: 54
        implicitHeight: 54
        hoverEnabled: true
        leftPadding: 0
        rightPadding: 0
        font.pixelSize: 25
        font.bold: false

        contentItem: Label {
            text: overlayButton.text
            color: overlayButton.enabled
                ? overlayButton.foregroundColor
                : root.withAlpha(overlayButton.foregroundColor, 0.42)
            font: overlayButton.font
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }

        background: Rectangle {
            radius: height / 2
            color: overlayButton.down
                ? (darkTheme ? "#ddd8cc" : theme.elevatedHover)
                : overlayButton.hovered
                    ? (darkTheme ? "#ffffff" : theme.elevated)
                    : overlayButton.surfaceColor
            border.color: darkTheme ? "#24ffffff" : theme.border
            border.width: 1
        }

        scale: overlayButton.down ? 0.96 : overlayButton.hovered ? 1.04 : 1.0
        Behavior on scale { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
    }

    component DetailThumbnailCorner: Canvas {
        property color fillColor: theme.bg
        implicitWidth: 12
        implicitHeight: 12
        antialiasing: true

        onFillColorChanged: requestPaint()
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()

        onPaint: {
            var context = getContext("2d")
            context.clearRect(0, 0, width, height)
            context.fillStyle = fillColor
            context.beginPath()
            context.moveTo(0, 0)
            context.lineTo(width, 0)
            context.quadraticCurveTo(0, 0, 0, height)
            context.closePath()
            context.fill()
        }
    }

    component DetailEpisodeCard: Item {
        id: detailEpisodeCard
        signal activated()
        property string title: ""
        property string episodeLabel: ""
        property string overview: ""
        property string imageUrl: ""
        property real progress: 0
        property bool selected: false

        implicitWidth: 330
        implicitHeight: 228
        scale: episodeMouse.containsMouse ? 1.018 : 1.0
        Behavior on scale { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }

        MouseArea {
            id: episodeMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: detailEpisodeCard.activated()
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 8

            Rectangle {
                id: episodeThumbnailFrame
                Layout.fillWidth: true
                Layout.preferredHeight: 154
                radius: 12
                color: theme.elevated
                border.color: "transparent"

                PosterImage {
                    anchors.fill: parent
                    radius: episodeThumbnailFrame.radius
                    imageUrl: detailEpisodeCard.imageUrl
                    fallbackText: detailEpisodeCard.title.length > 0 ? detailEpisodeCard.title[0] : "?"
                }

                Rectangle {
                    anchors.fill: parent
                    radius: parent.radius
                    color: episodeMouse.containsMouse ? "#12000000" : "transparent"
                }

                DetailThumbnailCorner {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    z: 2
                }

                DetailThumbnailCorner {
                    anchors.right: parent.right
                    anchors.top: parent.top
                    rotation: 90
                    z: 2
                }

                DetailThumbnailCorner {
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    rotation: 180
                    z: 2
                }

                DetailThumbnailCorner {
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    rotation: 270
                    z: 2
                }

                Rectangle {
                    anchors.fill: parent
                    radius: parent.radius
                    color: "transparent"
                    border.color: detailEpisodeCard.selected ? theme.primary : theme.border
                    border.width: detailEpisodeCard.selected ? 2 : 1
                    z: 3
                }

                Rectangle {
                    visible: detailEpisodeCard.selected
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.margins: 10
                    width: 27
                    height: 27
                    radius: width / 2
                    color: theme.primary
                    z: 4

                    Label {
                        anchors.centerIn: parent
                        text: "\u2713"
                        color: "#ffffff"
                        font.pixelSize: 17
                        font.bold: true
                    }
                }

                Rectangle {
                    visible: detailEpisodeCard.progress > 0
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    anchors.bottomMargin: 10
                    height: 4
                    radius: 2
                    color: root.withAlpha(darkTheme ? "#ffffff" : theme.text, 0.34)
                    z: 4

                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: parent.width * Math.min(100, detailEpisodeCard.progress) / 100
                        radius: 2
                        color: theme.primary
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                text: detailEpisodeCard.episodeLabel.length > 0
                    ? detailEpisodeCard.episodeLabel + " · " + detailEpisodeCard.title
                    : detailEpisodeCard.title
                color: theme.text
                font.pixelSize: 15
                font.bold: true
                elide: Text.ElideRight
            }

            Label {
                Layout.fillWidth: true
                Layout.fillHeight: true
                text: detailEpisodeCard.overview
                color: theme.muted
                font.pixelSize: 13
                lineHeight: 1.12
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                elide: Text.ElideRight
                verticalAlignment: Text.AlignTop
            }
        }
    }

    component DetailPage: Item {
        id: detailPage
        clip: true

        readonly property var backgroundImageUrls: appViewModel.selectedItemBackdropUrls
        readonly property string backgroundImageUrl: backgroundImageUrls.length > 0
                && backdropRotationIndex >= 0
                && backdropRotationIndex < backgroundImageUrls.length
            ? backgroundImageUrls[backdropRotationIndex]
            : ""
        readonly property real heroHeight: Math.max(540, Math.min(700, height * 0.78))
        readonly property color pageBackground: theme.bg
        readonly property color heroPrimaryText: theme.text
        readonly property color heroSecondaryText: darkTheme ? "#d9dde3" : theme.muted
        readonly property color floatingSurface: darkTheme ? "#f3eee4" : theme.surface
        readonly property color floatingForeground: darkTheme ? "#17191d" : theme.text
        readonly property color primaryActionSurface: darkTheme ? "#f5f3ef" : theme.primary
        readonly property color primaryActionHover: darkTheme ? "#ffffff" : theme.primaryHover
        readonly property color primaryActionPressed: darkTheme ? "#d9d7d2" : Qt.darker(theme.primary, 1.12)
        readonly property color primaryActionForeground: darkTheme ? "#202126" : "#ffffff"
        property string activeBackdropUrl: ""
        property string pendingBackdropUrl: ""
        property int activeBackdropLayer: 0
        property int pendingBackdropLayer: -1
        property int backdropRotationIndex: 0
        property string lastAnimatedItemId: ""

        function backdropIndexForUrl(url) {
            for (var index = 0; index < backgroundImageUrls.length; ++index) {
                if (backgroundImageUrls[index] === url) {
                    return index
                }
            }
            return -1
        }

        function synchronizeBackdrops() {
            if (backgroundImageUrls.length === 0) {
                backdropRotationIndex = 0
                requestBackdrop("")
                return
            }

            var activeIndex = backdropIndexForUrl(activeBackdropUrl)
            if (activeIndex >= 0) {
                backdropRotationIndex = activeIndex
                return
            }

            backdropRotationIndex = 0
            if (!appViewModel.episodeSwitching || activeBackdropUrl.length === 0) {
                requestBackdrop(backgroundImageUrl)
            }
        }

        function advanceBackdrop() {
            if (backgroundImageUrls.length < 2 || pendingBackdropLayer >= 0) {
                return
            }
            backdropRotationIndex = (backdropRotationIndex + 1) % backgroundImageUrls.length
            requestBackdrop(backgroundImageUrl)
        }

        function requestBackdrop(url) {
            var normalizedUrl = url || ""
            if (normalizedUrl.length === 0) {
                if (appViewModel.episodeSwitching && activeBackdropUrl.length > 0) {
                    return
                }
                pendingBackdropLayer = -1
                pendingBackdropUrl = ""
                activeBackdropUrl = ""
                detailBackdropA.source = ""
                detailBackdropB.source = ""
                return
            }
            if (normalizedUrl === activeBackdropUrl || normalizedUrl === pendingBackdropUrl) {
                return
            }

            pendingBackdropUrl = normalizedUrl
            pendingBackdropLayer = activeBackdropLayer === 0 ? 1 : 0
            if (pendingBackdropLayer === 0) {
                detailBackdropA.source = normalizedUrl
            } else {
                detailBackdropB.source = normalizedUrl
            }
        }

        function commitBackdrop(layer) {
            if (pendingBackdropLayer !== layer) {
                return
            }
            activeBackdropLayer = layer
            activeBackdropUrl = pendingBackdropUrl
            pendingBackdropUrl = ""
            pendingBackdropLayer = -1
        }

        function rejectBackdrop(layer) {
            if (pendingBackdropLayer !== layer) {
                return
            }
            pendingBackdropUrl = ""
            pendingBackdropLayer = -1
        }

        function animateSelectedItem() {
            if (appViewModel.selectedItemId.length === 0
                    || appViewModel.selectedItemId === lastAnimatedItemId) {
                return
            }
            lastAnimatedItemId = appViewModel.selectedItemId
            detailContentFade.restart()
        }

        onBackgroundImageUrlsChanged: {
            Qt.callLater(synchronizeBackdrops)
        }

        Component.onCompleted: {
            synchronizeBackdrops()
            animateSelectedItem()
        }

        Connections {
            target: appViewModel

            function onEpisodeSwitchingChanged() {
                if (!appViewModel.episodeSwitching) {
                    detailPage.synchronizeBackdrops()
                }
            }

            function onSelectedItemChanged() {
                detailPage.animateSelectedItem()
                Qt.callLater(function() {
                    episodeRail.revealSelectedEpisode(true)
                })
            }
        }

        NumberAnimation {
            id: detailContentFade
            target: detailHeroContent
            property: "opacity"
            from: 0.82
            to: 1.0
            duration: 180
            easing.type: Easing.OutCubic
        }

        Timer {
            interval: 12000
            repeat: true
            running: appViewModel.currentView === "details"
                && detailPage.backgroundImageUrls.length > 1
                && !appViewModel.episodeSwitching
            onTriggered: detailPage.advanceBackdrop()
        }

        Rectangle {
            anchors.fill: parent
            color: detailPage.pageBackground
        }

        Popup {
            id: detailSearchPopup
            x: Math.max(22, detailPage.width - width - 28)
            y: 26
            width: Math.min(460, detailPage.width - 44)
            height: 76
            padding: 14
            modal: false
            focus: true
            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

            background: Rectangle {
                radius: 18
                color: theme.surface
                border.color: theme.border
            }

            contentItem: MediaServerSearchBar {}
        }

        Flickable {
            id: detailFlick
            anchors.fill: parent
            contentWidth: width
            contentHeight: detailColumn.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            clip: true

            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
            }

            ColumnLayout {
                id: detailColumn
                width: detailFlick.width
                spacing: 0

                Item {
                    id: detailHero
                    Layout.fillWidth: true
                    Layout.preferredHeight: detailPage.heroHeight
                    clip: true

                    Image {
                        id: detailBackdropA
                        anchors.fill: parent
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        cache: true
                        opacity: detailPage.activeBackdropLayer === 0 ? 1 : 0

                        Behavior on opacity {
                            NumberAnimation { duration: 520; easing.type: Easing.InOutCubic }
                        }

                        onStatusChanged: {
                            if (status === Image.Ready) {
                                detailPage.commitBackdrop(0)
                            } else if (status === Image.Error) {
                                detailPage.rejectBackdrop(0)
                            }
                        }
                    }

                    Image {
                        id: detailBackdropB
                        anchors.fill: parent
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        cache: true
                        opacity: detailPage.activeBackdropLayer === 1 ? 1 : 0

                        Behavior on opacity {
                            NumberAnimation { duration: 520; easing.type: Easing.InOutCubic }
                        }

                        onStatusChanged: {
                            if (status === Image.Ready) {
                                detailPage.commitBackdrop(1)
                            } else if (status === Image.Error) {
                                detailPage.rejectBackdrop(1)
                            }
                        }
                    }

                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: Math.min(parent.width * 0.68, 820)
                        gradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop {
                                position: 0.0
                                color: root.withAlpha(detailPage.pageBackground, darkTheme ? 0.34 : 0.24)
                            }
                            GradientStop {
                                position: 0.42
                                color: root.withAlpha(detailPage.pageBackground, darkTheme ? 0.09 : 0.06)
                            }
                            GradientStop { position: 1.0; color: root.withAlpha(detailPage.pageBackground, 0.0) }
                        }
                    }

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: Math.min(parent.height * 0.68, 500)
                        gradient: Gradient {
                            GradientStop { position: 0.0; color: root.withAlpha(detailPage.pageBackground, 0.0) }
                            GradientStop {
                                position: 0.34
                                color: root.withAlpha(detailPage.pageBackground, darkTheme ? 0.15 : 0.12)
                            }
                            GradientStop {
                                position: 0.72
                                color: root.withAlpha(detailPage.pageBackground, darkTheme ? 0.72 : 0.78)
                            }
                            GradientStop { position: 1.0; color: detailPage.pageBackground }
                        }
                    }

                    Rectangle {
                        anchors.fill: parent
                        visible: detailPage.activeBackdropUrl.length === 0
                            && detailPage.pendingBackdropUrl.length === 0
                        color: theme.elevated
                    }

                    ThumbnailLoadingIcon {
                        anchors.centerIn: parent
                        running: detailPage.pendingBackdropLayer >= 0
                            && detailPage.activeBackdropUrl.length === 0
                        iconSize: 42
                    }

                    RowLayout {
                        id: detailHeroContent
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.leftMargin: 28
                        anchors.rightMargin: 30
                        anchors.bottomMargin: 24
                        spacing: 34

                        ColumnLayout {
                            Layout.preferredWidth: Math.max(360, Math.min(510, detailPage.width * 0.43))
                            Layout.alignment: Qt.AlignBottom
                            spacing: 14

                            Item {
                                Layout.fillWidth: true
                                Layout.preferredHeight: appViewModel.selectedItemLogoUrl.length > 0
                                        && detailTitleLogo.status !== Image.Error
                                    ? Math.max(92, Math.min(142, detailPage.heroHeight * 0.2))
                                    : detailTitleText.implicitHeight

                                Image {
                                    id: detailTitleLogo
                                    anchors.left: parent.left
                                    anchors.top: parent.top
                                    width: Math.min(parent.width, 450)
                                    height: parent.height
                                    source: appViewModel.selectedItemLogoUrl
                                    sourceSize.width: 900
                                    fillMode: Image.PreserveAspectFit
                                    horizontalAlignment: Image.AlignLeft
                                    verticalAlignment: Image.AlignVCenter
                                    asynchronous: true
                                    cache: true
                                    visible: status === Image.Ready
                                    opacity: status === Image.Ready ? 1 : 0

                                    Behavior on opacity {
                                        NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
                                    }
                                }

                                Label {
                                    id: detailTitleText
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: appViewModel.selectedItemName
                                    color: detailPage.heroPrimaryText
                                    font.pixelSize: Math.max(38, Math.min(62, detailPage.width * 0.046))
                                    font.weight: Font.DemiBold
                                    wrapMode: Text.WordWrap
                                    maximumLineCount: 2
                                    elide: Text.ElideRight
                                    style: Text.Raised
                                    styleColor: darkTheme ? "#55000000" : "#99ffffff"
                                    visible: detailTitleLogo.status !== Image.Ready
                                    opacity: visible ? 1 : 0

                                    Behavior on opacity {
                                        NumberAnimation { duration: 150; easing.type: Easing.OutCubic }
                                    }
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                visible: detailTitleLogo.status === Image.Ready
                                    && appViewModel.selectedItemType === "Episode"
                                    && appViewModel.selectedItemName.length > 0
                                text: appViewModel.selectedItemName
                                color: detailPage.heroPrimaryText
                                font.pixelSize: 20
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }

                            Button {
                                id: detailPlayButton
                                visible: !appViewModel.selectedItemIsSeries
                                enabled: !appViewModel.loading
                                Layout.fillWidth: true
                                Layout.maximumWidth: 430
                                Layout.preferredHeight: 58
                                hoverEnabled: true
                                onClicked: appViewModel.playSelectedItem()

                                contentItem: RowLayout {
                                    spacing: 12

                                    Label {
                                        Layout.leftMargin: 24
                                        text: "\u25b6"
                                        color: detailPage.primaryActionForeground
                                        font.pixelSize: 23
                                    }

                                    Label {
                                        text: appViewModel.selectedItemPlayedPercentage > 0
                                            ? t("action.continue")
                                            : t("action.play")
                                        color: detailPage.primaryActionForeground
                                        font.pixelSize: 18
                                        font.bold: true
                                    }

                                    Item { Layout.fillWidth: true }

                                    Label {
                                        visible: appViewModel.selectedItemPlayedPercentage > 0
                                        Layout.rightMargin: 24
                                        text: Math.round(appViewModel.selectedItemPlayedPercentage) + "%"
                                        color: root.withAlpha(detailPage.primaryActionForeground, 0.72)
                                        font.pixelSize: 15
                                        font.bold: true
                                    }
                                }

                                background: Rectangle {
                                    radius: height / 2
                                    color: detailPlayButton.down ? detailPage.primaryActionPressed
                                        : detailPlayButton.hovered ? detailPage.primaryActionHover
                                        : detailPage.primaryActionSurface
                                }
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignBottom
                            Layout.bottomMargin: 2
                            spacing: 8

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 9

                                Label {
                                    text: "\u2605"
                                    color: theme.danger
                                    font.pixelSize: 20
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: appViewModel.selectedItemMeta
                                    color: detailPage.heroPrimaryText
                                    font.pixelSize: 16
                                    font.bold: true
                                    wrapMode: Text.WordWrap
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                visible: appViewModel.selectedItemSeasonEpisode.length > 0
                                text: appViewModel.selectedItemSeasonEpisode
                                color: detailPage.heroSecondaryText
                                font.pixelSize: 15
                                font.bold: true
                            }

                            Label {
                                Layout.fillWidth: true
                                text: appViewModel.selectedItemOverview.length > 0
                                    ? appViewModel.selectedItemOverview
                                    : t("details.noOverview")
                                color: detailPage.heroSecondaryText
                                font.pixelSize: 15
                                lineHeight: 1.12
                                wrapMode: Text.WordWrap
                                maximumLineCount: 4
                                elide: Text.ElideRight
                            }
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 28
                    Layout.rightMargin: 28
                    Layout.topMargin: 8
                    visible: appViewModel.selectedItemHasSeriesEpisodes
                    spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        Label {
                            text: appViewModel.selectedSeasonName.length > 0
                                ? appViewModel.selectedSeasonName
                                : t("details.seasonsEpisodes")
                            color: theme.text
                            font.pixelSize: 22
                            font.bold: true
                        }

                        Label {
                            text: "\u2304"
                            color: theme.text
                            font.pixelSize: 22
                        }

                        Item { Layout.fillWidth: true }
                    }

                    ListView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: appViewModel.seriesSeasons.count > 1 ? 44 : 0
                        visible: appViewModel.seriesSeasons.count > 1
                        orientation: ListView.Horizontal
                        boundsBehavior: Flickable.StopAtBounds
                        spacing: 10
                        model: appViewModel.seriesSeasons
                        clip: true

                        delegate: SeasonPill {
                            width: Math.min(190, Math.max(104, model.name.length * 9 + 34))
                            height: 38
                            title: model.name
                            selected: model.itemId === appViewModel.selectedSeasonId
                            onActivated: appViewModel.selectSeason(index)
                        }
                    }

                    ListView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: appViewModel.seriesEpisodes.count > 0 ? 46 : 0
                        visible: appViewModel.seriesEpisodes.count > 0
                        orientation: ListView.Horizontal
                        boundsBehavior: Flickable.StopAtBounds
                        spacing: 10
                        model: appViewModel.seriesEpisodes
                        clip: true

                        delegate: Rectangle {
                            readonly property bool currentEpisode: model.itemId === appViewModel.selectedItemId
                            width: 38
                            height: 38
                            radius: width / 2
                            color: currentEpisode ? (darkTheme ? "#f4f2ed" : theme.primary)
                                : episodeNumberMouse.containsMouse ? theme.elevatedHover
                                : theme.elevated
                            border.color: currentEpisode ? (darkTheme ? "#ffffff" : theme.primary) : theme.border

                            Behavior on color {
                                ColorAnimation { duration: 140; easing.type: Easing.OutCubic }
                            }

                            Label {
                                anchors.centerIn: parent
                                text: model.indexNumber > 0 ? model.indexNumber : index + 1
                                color: parent.currentEpisode
                                    ? (darkTheme ? "#17191d" : "#ffffff")
                                    : theme.text
                                font.pixelSize: 15
                                font.bold: parent.currentEpisode
                            }

                            MouseArea {
                                id: episodeNumberMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: appViewModel.openEpisode(index)
                            }
                        }
                    }

                    ListView {
                        id: episodeRail
                        property real episodeCardWidth: Math.max(286, Math.min(350, detailPage.width * 0.285))
                        property bool selectionPositionInitialized: false

                        function revealSelectedEpisode(animated) {
                            var selectedRow = appViewModel.seriesEpisodes.indexOfItemId(appViewModel.selectedItemId)
                            if (selectedRow < 0 || width <= 0 || contentWidth <= 0) {
                                return
                            }

                            var centeredPosition = selectedRow * (episodeCardWidth + spacing)
                                - (width - episodeCardWidth) / 2
                            var maximumPosition = Math.max(0, contentWidth - width)
                            var targetPosition = Math.max(0, Math.min(maximumPosition, centeredPosition))
                            if (!selectionPositionInitialized || !animated) {
                                episodeRailScroll.stop()
                                contentX = targetPosition
                                selectionPositionInitialized = true
                                return
                            }
                            if (Math.abs(contentX - targetPosition) < 1) {
                                return
                            }

                            episodeRailScroll.stop()
                            episodeRailScroll.from = contentX
                            episodeRailScroll.to = targetPosition
                            episodeRailScroll.duration = Math.max(180,
                                Math.min(420, 180 + Math.abs(targetPosition - contentX) * 0.16))
                            episodeRailScroll.start()
                        }

                        Layout.fillWidth: true
                        Layout.preferredHeight: appViewModel.seriesEpisodes.count > 0 ? 244 : 0
                        visible: appViewModel.seriesEpisodes.count > 0
                        orientation: ListView.Horizontal
                        boundsBehavior: Flickable.StopAtBounds
                        spacing: 20
                        model: appViewModel.seriesEpisodes
                        clip: true

                        onCountChanged: {
                            if (count === 0) {
                                selectionPositionInitialized = false
                            } else {
                                Qt.callLater(function() {
                                    episodeRail.revealSelectedEpisode(false)
                                })
                            }
                        }

                        NumberAnimation {
                            id: episodeRailScroll
                            target: episodeRail
                            property: "contentX"
                            easing.type: Easing.InOutCubic
                        }

                        delegate: DetailEpisodeCard {
                            width: episodeRail.episodeCardWidth
                            height: 228
                            title: model.name
                            episodeLabel: appViewModel.formatSeasonEpisode(model.parentIndexNumber, model.indexNumber)
                            overview: model.overview
                            imageUrl: model.imageUrl
                            progress: model.playedPercentage
                            selected: model.itemId === appViewModel.selectedItemId
                            onActivated: appViewModel.openEpisode(index)
                        }
                    }

                    MutedText {
                        Layout.fillWidth: true
                        visible: appViewModel.seriesSeasons.count === 0 && !appViewModel.loading
                        text: t("details.noSeasons")
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 28
                    Layout.rightMargin: 28
                    Layout.topMargin: 22
                    Layout.bottomMargin: 30
                    spacing: 14

                    SectionHeader {
                        title: t("details.castCrew")
                        subtitle: appViewModel.selectedItemPeopleModel.count > 0 ? "" : t("details.noCast")
                    }

                    ListView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: appViewModel.selectedItemPeopleModel.count > 0 ? 218 : 0
                        visible: appViewModel.selectedItemPeopleModel.count > 0
                        orientation: ListView.Horizontal
                        boundsBehavior: Flickable.StopAtBounds
                        spacing: 14
                        model: appViewModel.selectedItemPeopleModel
                        clip: true

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

        DetailOverlayButton {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: 26
            text: "\u2190"
            z: 20
            onClicked: appViewModel.mediaDetailsBack()
        }

        Rectangle {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.topMargin: 26
            anchors.rightMargin: 26
            width: 146
            height: 56
            radius: height / 2
            color: detailPage.floatingSurface
            border.color: darkTheme ? "#24ffffff" : theme.border
            z: 20

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 10
                spacing: 2

                Button {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    hoverEnabled: true
                    onClicked: detailSearchPopup.open()

                    contentItem: Label {
                        text: "\uD83D\uDD0D"
                        color: detailPage.floatingForeground
                        font.pixelSize: 23
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    background: Rectangle {
                        radius: height / 2
                        color: parent.hovered
                            ? (darkTheme ? "#18ffffff" : root.withAlpha(theme.primary, 0.08))
                            : "transparent"
                    }
                }

                Button {
                    Layout.preferredWidth: 56
                    Layout.fillHeight: true
                    hoverEnabled: true
                    onClicked: overviewDialog.open()

                    contentItem: Label {
                        text: "\u2026"
                        color: detailPage.floatingForeground
                        font.pixelSize: 28
                        font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    background: Rectangle {
                        radius: height / 2
                        color: parent.hovered
                            ? (darkTheme ? "#18ffffff" : root.withAlpha(theme.primary, 0.08))
                            : "transparent"
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
        readonly property bool traditionalLayout: root.useTraditionalPlayer
        property int traditionalChromeHeight: 136
        property int topChromeHeight: traditionalLayout ? 1 : 0
        property int topChromeMargin: 0
        property int bottomChromeHeight: traditionalLayout ? traditionalChromeHeight : 196
        property int bottomChromeMargin: 24
        property color chromePanelColor: "#990a0d12"
        property bool seekLoadingActive: false
        property bool progressSeekActive: false
        property bool progressSeekDragging: false
        property real progressSeekPosition: 0
        readonly property real displayedPlaybackPosition: progressSeekActive
            ? progressSeekPosition : mpvVideo.position
        property bool rawPlaybackLoading: mpvVideo.loading || mpvVideo.buffering || mpvVideo.seeking || seekLoadingActive
        readonly property bool audioPlaybackLoading: appViewModel.webDavAudioPlaybackActive && rawPlaybackLoading
        readonly property bool audioPaused: mpvVideo.paused
        readonly property real playbackPosition: mpvVideo.position
        readonly property real audioPosition: mpvVideo.position
        readonly property real audioDuration: mpvVideo.duration
        readonly property string audioDisplayTitle: mpvVideo.audioTitle.length > 0
            ? mpvVideo.audioTitle : appViewModel.webDavAudioCurrentName
        readonly property string audioDisplayArtist: mpvVideo.audioArtist
        readonly property url audioCoverUrl: mpvVideo.audioCoverUrl
        readonly property string audioMetadataDetails: {
            var parts = []
            if (mpvVideo.audioAlbum.length > 0) parts.push(mpvVideo.audioAlbum)
            if (mpvVideo.audioGenre.length > 0) parts.push(mpvVideo.audioGenre)
            if (mpvVideo.audioDate.length > 0) parts.push(mpvVideo.audioDate)
            if (mpvVideo.audioTrack.length > 0) parts.push("#" + mpvVideo.audioTrack)
            return parts.join("  \u00B7  ")
        }
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
            stopCurrentPlayback(exitPositionSeconds)
        }

        function exitPlaybackImmediately() {
            closeTrackMenu(false)
            closeIptvChannelList(false)
            stopCurrentPlayback(mpvVideo.position)
        }

        function stopCurrentPlayback(positionSeconds) {
            appViewModel.reportPlaybackStopped(positionSeconds)
            mpvVideo.stop()
            root.exitPlayerFullscreen()
            exitConfirmVisible = false
            videoInfoVisible = false
            trackMenuVisible = false
            iptvChannelListVisible = false
            appViewModel.closePlayerToDetails()
        }

        function stopAudioPlayback() {
            stopCurrentPlayback(mpvVideo.position)
        }

        function toggleAudioPause() {
            if (audioPlaybackLoading) {
                return
            }
            mpvVideo.togglePause()
            appViewModel.reportPlaybackProgress(mpvVideo.position, mpvVideo.duration, mpvVideo.paused)
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

        function beginProgressSeek(position) {
            progressSeekActive = true
            progressSeekDragging = true
            progressSeekPosition = Math.max(0, Math.min(mpvVideo.duration, position))
            revealControls()
        }

        function updateProgressSeek(position) {
            if (!progressSeekDragging) {
                return
            }
            progressSeekPosition = Math.max(0, Math.min(mpvVideo.duration, position))
            revealControls()
        }

        function commitProgressSeek() {
            if (!progressSeekDragging) {
                return
            }
            progressSeekDragging = false
            var target = progressSeekPosition
            if (Math.abs(target - mpvVideo.position) < 0.05) {
                progressSeekActive = false
                return
            }
            mpvVideo.seekAbsolute(target)
            beginSeekLoading()
            appViewModel.reportPlaybackProgress(target, mpvVideo.duration, mpvVideo.paused)
            revealControls()
        }

        function finishSeekLoading() {
            progressSeekActive = false
            progressSeekDragging = false
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
            if (appViewModel.webDavAudioPlaybackActive && event.key === Qt.Key_Right) {
                appViewModel.skipWebDavAudioTrack(1)
                event.accepted = true
                return
            }
            if (appViewModel.webDavAudioPlaybackActive && event.key === Qt.Key_Left) {
                appViewModel.skipWebDavAudioTrack(-1)
                event.accepted = true
                return
            }
            if (event.key === Qt.Key_Escape && iptvChannelListVisible) {
                closeIptvChannelList()
                event.accepted = true
            } else if (event.key === Qt.Key_Escape && trackMenuVisible) {
                closeTrackMenu()
                event.accepted = true
            } else if (event.key === Qt.Key_Space) {
                mpvVideo.togglePause()
                appViewModel.reportPlaybackProgress(mpvVideo.position, mpvVideo.duration, mpvVideo.paused)
                playerPage.revealControls()
                event.accepted = true
            } else if (event.key === Qt.Key_Right) {
                mpvVideo.seekRelative(15)
                playerPage.beginSeekLoading()
                appViewModel.reportPlaybackProgress(mpvVideo.position + 15, mpvVideo.duration, mpvVideo.paused)
                playerPage.revealControls()
                event.accepted = true
            } else if (event.key === Qt.Key_Left) {
                mpvVideo.seekRelative(-15)
                playerPage.beginSeekLoading()
                appViewModel.reportPlaybackProgress(Math.max(0, mpvVideo.position - 15), mpvVideo.duration, mpvVideo.paused)
                playerPage.revealControls()
                event.accepted = true
            } else if (event.key === Qt.Key_F) {
                playerPage.toggleFullscreen()
                event.accepted = true
            } else if (event.key === Qt.Key_Escape) {
                if (appViewModel.webDavAudioPlaybackActive) {
                    appViewModel.minimizeWebDavAudioPlayer()
                } else if (playerPage.immersive || root.visibility === Window.FullScreen) {
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

        function liveFrameRateText(frameRate) {
            if (!Number.isFinite(frameRate) || frameRate <= 0) {
                return "-- FPS"
            }
            return Number(frameRate).toFixed(frameRate >= 100 ? 0 : 1) + " FPS"
        }

        function networkSpeedText(bytesPerSecond) {
            if (!Number.isFinite(bytesPerSecond) || bytesPerSecond < 0) {
                return "--"
            }
            return root.formatBytes(bytesPerSecond) + "/s"
        }

        function playbackMetricsText() {
            return liveFrameRateText(mpvVideo.currentFrameRate)
                + "  |  " + t("player.networkSpeedShort") + " "
                + networkSpeedText(mpvVideo.networkSpeedBytesPerSecond)
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

        function loadExternalSubtitle(url) {
            closeTrackMenu(false)
            mpvVideo.loadExternalSubtitle(url)
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
                return mpvVideo.subtitleTracks.count + 2
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
                    playerPage.progressSeekActive = false
                    playerPage.progressSeekDragging = false
                    playerPage.videoInfoVisible = false
                    playerPage.trackMenuVisible = false
                    playerPage.iptvChannelListVisible = false
                    playbackLoadingDelay.stop()
                    seekLoadingTimeout.stop()
                }
            }
        }

        Rectangle {
            anchors.fill: parent
            visible: !appViewModel.webDavAudioPlaybackActive
            color: "#000000"
        }

        MpvVideoItem {
            id: mpvVideo
            anchors.fill: parent
            audioOnly: appViewModel.webDavAudioPlaybackActive
            startPosition: appViewModel.currentPlaybackStartSeconds
            preferredSubtitleStreamIndex: appViewModel.currentPlaybackSubtitleStreamIndex
            httpUsername: appViewModel.playbackHttpUsername
            httpPassword: appViewModel.playbackHttpPassword
            allowInsecureTls: appViewModel.playbackAllowInsecureTls
            source: appViewModel.currentPlaybackUrl
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
            onPlaybackRestarted: {
                playerPage.finishSeekLoading()
                appViewModel.reportPlaybackStarted()
            }
            onPlaybackEnded: function(positionSeconds, reachedEnd, failed) {
                appViewModel.reportPlaybackEnded(positionSeconds, reachedEnd, failed)
                appViewModel.advanceWebDavAudioPlayback(reachedEnd, failed)
            }
            onPlaybackStateChanged: {
                if (!appViewModel.webDavAudioPlaybackActive
                        && playerPage.rawPlaybackLoading && appViewModel.currentView === "player") {
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
                appViewModel.reportPlaybackProgress(position, duration, paused)
            }
            Component.onCompleted: {
                play()
            }
        }

        Rectangle {
            id: audioPlayerSurface
            anchors.fill: parent
            visible: appViewModel.webDavAudioPlaybackActive
            color: theme.bg
            z: 2

            RowLayout {
                anchors.fill: parent
                anchors.margins: Math.max(22, Math.min(42, audioPlayerSurface.width * 0.04))
                spacing: 30

                ColumnLayout {
                    id: audioNowPlaying
                    readonly property real artworkSize: Math.min(300, Math.max(210, width * 0.42), height * 0.48)
                    Layout.preferredWidth: Math.min(680, Math.max(510, parent.width * 0.57))
                    Layout.fillHeight: true
                    spacing: 16

                    Item { Layout.fillHeight: true }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignHCenter
                        spacing: Math.max(22, width * 0.04)

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 210
                            Layout.alignment: Qt.AlignVCenter
                            spacing: 8

                            MutedText {
                                Layout.fillWidth: true
                                text: appViewModel.currentServerName
                                font.pixelSize: 11
                                font.bold: true
                                elide: Text.ElideRight
                            }

                            Label {
                                Layout.fillWidth: true
                                text: playerPage.audioDisplayTitle
                                color: theme.text
                                font.pixelSize: 24
                                font.bold: true
                                wrapMode: Text.Wrap
                                maximumLineCount: 3
                                elide: Text.ElideRight
                            }

                            MutedText {
                                Layout.fillWidth: true
                                visible: playerPage.audioDisplayArtist.length > 0
                                text: playerPage.audioDisplayArtist
                                font.pixelSize: 15
                                font.bold: true
                                elide: Text.ElideRight
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 1
                                Layout.topMargin: 4
                                Layout.bottomMargin: 4
                                color: theme.border
                            }

                            MutedText {
                                Layout.fillWidth: true
                                visible: playerPage.audioMetadataDetails.length > 0
                                text: playerPage.audioMetadataDetails
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                                maximumLineCount: 3
                                elide: Text.ElideRight
                            }

                            MutedText {
                                Layout.fillWidth: true
                                text: (appViewModel.webDavAudioCurrentIndex + 1)
                                    + " / " + appViewModel.webDavAudioQueueCount
                                font.pixelSize: 12
                            }
                        }

                        Rectangle {
                            id: audioCoverFrame
                            readonly property real boundedSize: audioNowPlaying.artworkSize

                            Layout.minimumWidth: boundedSize
                            Layout.preferredWidth: boundedSize
                            Layout.maximumWidth: boundedSize
                            Layout.minimumHeight: boundedSize
                            Layout.preferredHeight: boundedSize
                            Layout.maximumHeight: boundedSize
                            Layout.alignment: Qt.AlignVCenter
                            radius: 14
                            color: root.withAlpha(theme.primary, darkTheme ? 0.20 : 0.10)
                            border.color: root.withAlpha(theme.primary, 0.48)
                            border.width: 1
                            clip: true

                            RoundedCoverImage {
                                id: audioCoverImage
                                anchors.fill: parent
                                anchors.margins: 1
                                source: playerPage.audioCoverUrl
                                cornerRadius: 13
                                visible: status === Image.Ready
                            }

                            Column {
                                anchors.centerIn: parent
                                width: parent.width - 32
                                spacing: 10
                                visible: audioCoverImage.status !== Image.Ready

                                Label {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: "\u266B"
                                    color: theme.primary
                                    font.pixelSize: Math.max(58, Math.min(86, parent.width * 0.34))
                                    font.bold: true
                                }

                                Label {
                                    width: parent.width
                                    text: mpvVideo.audioAlbum.length > 0
                                        ? mpvVideo.audioAlbum : appViewModel.currentServerName
                                    color: theme.muted
                                    font.pixelSize: 12
                                    font.bold: true
                                    horizontalAlignment: Text.AlignHCenter
                                    elide: Text.ElideRight
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 9

                        MutedText { text: playerPage.formatTime(playerPage.displayedPlaybackPosition) }

                        Slider {
                            id: audioProgressSlider
                            Layout.fillWidth: true
                            from: 0
                            to: Math.max(1, mpvVideo.duration)
                            value: playerPage.displayedPlaybackPosition
                            onPressedChanged: {
                                if (pressed) {
                                    playerPage.beginProgressSeek(value)
                                } else {
                                    playerPage.commitProgressSeek()
                                }
                            }
                            onMoved: playerPage.updateProgressSeek(value)
                        }

                        MutedText { text: playerPage.formatTime(mpvVideo.duration) }
                    }

                    RowLayout {
                        Layout.alignment: Qt.AlignHCenter
                        spacing: 18

                        RowLayout {
                            spacing: 10

                            AudioTransportButton {
                                iconKind: "previous"
                                enabled: appViewModel.webDavAudioQueueCount > 1
                                    && (appViewModel.webDavAudioRepeatMode === "all"
                                        || appViewModel.webDavAudioCurrentIndex > 0)
                                Accessible.name: t("action.previous")
                                ToolTip.visible: hovered
                                ToolTip.text: t("action.previous")
                                onClicked: appViewModel.skipWebDavAudioTrack(-1)
                            }

                            AudioTransportButton {
                                loading: playerPage.audioPlaybackLoading
                                iconKind: playerPage.audioPaused ? "play" : "pause"
                                primaryAction: true
                                Accessible.name: loading ? t("player.loading")
                                    : playerPage.audioPaused ? t("action.resume") : t("action.pause")
                                ToolTip.visible: hovered
                                ToolTip.text: Accessible.name
                                onClicked: playerPage.toggleAudioPause()
                            }

                            AudioTransportButton {
                                iconKind: "next"
                                enabled: appViewModel.webDavAudioQueueCount > 1
                                    && (appViewModel.webDavAudioRepeatMode === "all"
                                        || appViewModel.webDavAudioCurrentIndex + 1 < appViewModel.webDavAudioQueueCount)
                                Accessible.name: t("action.next")
                                ToolTip.visible: hovered
                                ToolTip.text: t("action.next")
                                onClicked: appViewModel.skipWebDavAudioTrack(1)
                            }
                        }

                        AudioRepeatModeSwitch {}
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        BodyText { text: t("player.volume"); color: theme.muted }

                        Slider {
                            Layout.fillWidth: true
                            from: 0
                            to: 100
                            value: mpvVideo.volume
                            onMoved: mpvVideo.setVolume(value)
                        }
                    }

                    Item { Layout.fillHeight: true }
                }

                Rectangle {
                    Layout.fillHeight: true
                    Layout.preferredWidth: 1
                    color: theme.border
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 14

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            Label {
                                Layout.fillWidth: true
                                text: t("webdav.modeAudio")
                                color: theme.text
                                font.pixelSize: 19
                                font.bold: true
                                elide: Text.ElideRight
                            }

                            MutedText {
                                Layout.fillWidth: true
                                text: appViewModel.webDavCurrentPath
                                elide: Text.ElideMiddle
                            }
                        }

                        Label {
                            text: appViewModel.webDavAudioQueueCount + " " + t("webdav.audio")
                            color: theme.primary
                            font.pixelSize: 13
                            font.bold: true
                        }

                        ModernButton {
                            text: t("action.exitPlayback")
                            danger: true
                            onClicked: playerPage.exitPlaybackImmediately()
                        }
                    }

                    ListView {
                        id: audioQueueList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 7
                        boundsBehavior: Flickable.StopAtBounds
                        model: appViewModel.webDavAudioPlaybackActive ? appViewModel.webDavItems : null
                        currentIndex: appViewModel.webDavAudioCurrentIndex
                        onCurrentIndexChanged: {
                            if (currentIndex >= 0) {
                                positionViewAtIndex(currentIndex, ListView.Contain)
                            }
                        }

                        delegate: Rectangle {
                            id: audioQueueRow
                            required property int index
                            required property string name
                            required property real bytes
                            width: audioQueueList.width
                            height: 58
                            radius: 8
                            color: index === appViewModel.webDavAudioCurrentIndex
                                ? root.withAlpha(theme.primary, darkTheme ? 0.24 : 0.12)
                                : audioQueueMouse.containsMouse ? theme.elevatedHover : theme.elevated
                            border.color: index === appViewModel.webDavAudioCurrentIndex
                                ? root.withAlpha(theme.primary, 0.72)
                                : theme.border

                            MouseArea {
                                id: audioQueueMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: appViewModel.startWebDavAudioPlayback(audioQueueRow.index)
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 12
                                spacing: 10

                                Label {
                                    text: (audioQueueRow.index < 9 ? "0" : "") + (audioQueueRow.index + 1)
                                    color: audioQueueRow.index === appViewModel.webDavAudioCurrentIndex ? theme.primary : theme.subtle
                                    font.pixelSize: 12
                                    font.bold: true
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2

                                    Label {
                                        Layout.fillWidth: true
                                        text: audioQueueRow.name
                                        color: theme.text
                                        font.pixelSize: 13
                                        font.bold: audioQueueRow.index === appViewModel.webDavAudioCurrentIndex
                                        elide: Text.ElideMiddle
                                    }

                                    MutedText {
                                        Layout.fillWidth: true
                                        text: audioQueueRow.bytes >= 0 ? root.formatBytes(audioQueueRow.bytes) : t("webdav.audio")
                                        font.pixelSize: 11
                                        elide: Text.ElideRight
                                    }
                                }

                                Label {
                                    visible: audioQueueRow.index === appViewModel.webDavAudioCurrentIndex
                                    text: "\u25B6"
                                    color: theme.primary
                                }
                            }
                        }
                    }
                }
            }
        }

        MouseArea {
            anchors.fill: mpvVideo
            visible: !appViewModel.webDavAudioPlaybackActive
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
            onTriggered: appViewModel.reportPlaybackProgress(mpvVideo.position, mpvVideo.duration, mpvVideo.paused)
        }

        Timer {
            id: playbackLoadingDelay
            interval: 300
            repeat: false
            onTriggered: {
                playerPage.playbackLoadingVisible = !appViewModel.webDavAudioPlaybackActive
                    && playerPage.rawPlaybackLoading && appViewModel.currentView === "player"
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
            visible: appViewModel.currentView === "player" && root.visible
                && !appViewModel.webDavAudioPlaybackActive && playerPage.playbackLoadingVisible

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
                    readonly property bool determinateProgress: mpvVideo.buffering
                        && mpvVideo.bufferingProgress > 0 && mpvVideo.bufferingProgress < 100
                    readonly property real normalizedProgress: Math.max(0,
                        Math.min(1, mpvVideo.bufferingProgress / 100))

                    anchors.centerIn: parent
                    width: Math.min(parent.width - 48, 380)
                    height: 136
                    radius: 12
                    color: "#c20a0d12"
                    border.color: "#617b8da1"
                    opacity: playerLoadingWindow.visible ? 1 : 0
                    scale: playerLoadingWindow.visible ? 1 : 0.97

                    Behavior on opacity {
                        NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
                    }
                    Behavior on scale {
                        NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
                    }

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 18
                        anchors.rightMargin: 18
                        anchors.topMargin: 16
                        anchors.bottomMargin: 14
                        spacing: 9

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 14

                            ThumbnailLoadingIcon {
                                Layout.preferredWidth: 46
                                Layout.preferredHeight: 46
                                iconSize: 46
                                running: playerLoadingWindow.visible
                                accentColor: "#6aa0ff"
                                backgroundVisible: false
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 4

                                Label {
                                    Layout.fillWidth: true
                                    text: playerPage.playbackLoadingTitle()
                                    color: "#ffffff"
                                    font.pixelSize: 18
                                    font.bold: true
                                    elide: Text.ElideRight
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: appViewModel.selectedItemName.length > 0
                                        ? appViewModel.selectedItemName : t("player.networkHint")
                                    color: "#b8c5d4"
                                    font.pixelSize: 12
                                    elide: Text.ElideMiddle
                                }
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10

                            Label {
                                Layout.fillWidth: true
                                text: t("player.networkHint")
                                color: "#91a0b2"
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }

                            Label {
                                visible: playbackLoadingCard.determinateProgress
                                text: Math.round(mpvVideo.bufferingProgress) + "%"
                                color: "#dce8f8"
                                font.pixelSize: 11
                                font.bold: true
                            }
                        }

                        Rectangle {
                            id: playbackLoadingTrack
                            Layout.fillWidth: true
                            Layout.preferredHeight: 4
                            radius: 2
                            color: "#2effffff"
                            clip: true

                            Rectangle {
                                visible: playbackLoadingCard.determinateProgress
                                x: 0
                                width: playbackLoadingTrack.width * playbackLoadingCard.normalizedProgress
                                height: parent.height
                                radius: parent.radius
                                color: "#6aa0ff"
                            }

                            Rectangle {
                                id: playbackLoadingSweep
                                visible: !playbackLoadingCard.determinateProgress
                                x: 0
                                width: Math.max(48, playbackLoadingTrack.width * 0.24)
                                height: parent.height
                                radius: parent.radius
                                color: "#7ba9ff"

                                NumberAnimation on x {
                                    running: playerLoadingWindow.visible
                                        && !playbackLoadingCard.determinateProgress
                                    from: -playbackLoadingSweep.width
                                    to: playbackLoadingTrack.width
                                    duration: 1150
                                    loops: Animation.Infinite
                                    easing.type: Easing.InOutSine
                                }
                            }
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
                var panelHeight = 310
                var margin = 24
                var localX = Math.max(margin, playerPage.width - panelWidth - margin)
                var localY = Math.min(playerPage.height - panelHeight - margin,
                    playerPage.topChromeMargin + playerPage.topChromeHeight + 18)
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
                            text: t("player.sourceFrameRate")
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
                            text: t("player.frameRate")
                            color: "#aeb8c6"
                        }
                        BodyText {
                            Layout.fillWidth: true
                            text: playerPage.liveFrameRateText(mpvVideo.currentFrameRate)
                            color: "#ffffff"
                            font.bold: true
                            elide: Text.ElideRight
                        }

                        MutedText {
                            text: t("player.networkSpeed")
                            color: "#aeb8c6"
                        }
                        BodyText {
                            Layout.fillWidth: true
                            text: playerPage.networkSpeedText(mpvVideo.networkSpeedBytesPerSecond)
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
                var localY = playerPage.height - playerPage.bottomChromeHeight
                    - playerPage.bottomChromeMargin - panelHeight - 12
                localY = Math.max(playerPage.topChromeMargin + playerPage.topChromeHeight + 12, localY)
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

                    ModernButton {
                        Layout.fillWidth: true
                        visible: playerPage.trackMenuMode === "subtitle"
                        text: t("player.loadSubtitle")
                        onClicked: {
                            playerPage.closeTrackMenu(false)
                            externalSubtitleDialog.open()
                        }
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
                var availableHeight = playerPage.height - playerPage.topChromeMargin - playerPage.topChromeHeight
                    - playerPage.bottomChromeHeight - playerPage.bottomChromeMargin - margin * 2
                if (availableHeight < 220) {
                    availableHeight = Math.max(160, playerPage.height - margin * 2)
                }
                var panelHeight = Math.min(620, availableHeight)
                var playerOrigin = playerPage.mapToGlobal(0, 0)
                var localX = Math.max(margin, playerPage.width - panelWidth - margin)
                var preferredY = playerPage.topChromeMargin + playerPage.topChromeHeight + margin
                var maxY = playerPage.height - playerPage.bottomChromeHeight
                    - playerPage.bottomChromeMargin - panelHeight - margin
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
            visible: appViewModel.currentView === "player" && root.visible
                && playerPage.traditionalLayout
                && (playerPage.exitConfirmVisible
                    || (!appViewModel.webDavAudioPlaybackActive && playerPage.controlsVisible))

            function syncChromeGeometry() {
                if (playerPage.width <= 0 || playerPage.height <= 0) {
                    return
                }
                var origin = playerPage.mapToGlobal(0, 0)
                if (playerPage.traditionalLayout) {
                    var panelWidth = Math.min(Math.max(320, playerPage.width - 36), 560)
                    var panelHeight = playerPage.traditionalChromeHeight
                    var panelX = Math.max(18, (playerPage.width - panelWidth) / 2)
                    var panelY = Math.max(18, playerPage.height - panelHeight - playerPage.bottomChromeMargin)
                    x = Math.round(origin.x + panelX)
                    y = Math.round(origin.y + panelY)
                    width = Math.round(Math.min(panelWidth, playerPage.width - panelX * 2))
                    height = Math.round(panelHeight)
                    return
                }
                var modernPanelWidth = Math.min(1040, Math.max(1, playerPage.width - 48))
                var modernPanelX = Math.max(24, (playerPage.width - modernPanelWidth) / 2)
                x = Math.round(origin.x + modernPanelX)
                y = Math.round(origin.y + playerPage.topChromeMargin)
                width = Math.round(Math.min(modernPanelWidth, playerPage.width - modernPanelX * 2))
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
                    visible: !playerPage.traditionalLayout
                    radius: 10
                    color: playerPage.chromePanelColor
                    border.color: "#44343b46"
                    clip: true

                    HoverHandler {
                        onHoveredChanged: if (hovered) playerPage.revealControls()
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 20
                        anchors.rightMargin: 20
                        spacing: 12

                        ModernButton {
                            text: t("action.exitPlayback")
                            danger: true
                            onClicked: playerPage.exitPlaybackImmediately()
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

                Rectangle {
                    id: traditionalPlayerControls
                    visible: playerPage.traditionalLayout
                    anchors.fill: parent
                    radius: 5
                    color: "#e31c2229"
                    border.color: "#7a8290a0"

                        HoverHandler {
                            onHoveredChanged: if (hovered) playerPage.revealControls()
                        }

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            anchors.topMargin: 8
                            anchors.bottomMargin: 8
                            spacing: 5

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 7

                                PlayerChromeButton {
                                    id: traditionalExitButton
                                    compact: true
                                    iconText: "<"
                                    danger: true
                                    Accessible.name: t("action.exitPlayback")
                                    onClicked: playerPage.exitPlaybackImmediately()
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: playerPage.exitConfirmVisible
                                        ? t("dialog.exitPlaybackPrompt") : appViewModel.selectedItemName
                                    color: "#e9edf3"
                                    font.pixelSize: 12
                                    elide: Text.ElideMiddle
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }

                                MutedText {
                                    visible: !playerPage.exitConfirmVisible
                                    text: playerPage.playbackMetricsText()
                                    color: "#bdc7d4"
                                    font.pixelSize: 10
                                    horizontalAlignment: Text.AlignRight
                                }

                                PlayerChromeButton {
                                    id: traditionalFullscreenButton
                                    compact: true
                                    visible: !playerPage.exitConfirmVisible
                                    iconText: playerPage.immersive ? "[]" : "[ ]"
                                    Accessible.name: playerPage.immersive ? t("action.exitFullscreen") : t("action.fullscreen")
                                    onClicked: playerPage.toggleFullscreen()
                                }

                                PlayerChromeButton {
                                    id: traditionalCancelExitButton
                                    compact: true
                                    visible: playerPage.exitConfirmVisible
                                    iconText: "X"
                                    Accessible.name: t("action.cancel")
                                    onClicked: playerPage.cancelExitPlayback()
                                }

                                PlayerChromeButton {
                                    id: traditionalConfirmExitButton
                                    compact: true
                                    visible: playerPage.exitConfirmVisible
                                    iconText: "OK"
                                    danger: true
                                    Accessible.name: t("action.exitPlayback")
                                    onClicked: playerPage.confirmExitPlayback()
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 4

                                MutedText {
                                    text: playerPage.exitConfirmVisible ? "" : "1/1"
                                    color: "#b3bdc9"
                                    font.pixelSize: 10
                                }

                                Item { Layout.fillWidth: true }

                                PlayerTransportButton {
                                    id: traditionalRewindButton
                                    iconKind: "previous"
                                    badgeText: "15"
                                    Accessible.name: t("action.rewind15")
                                    onClicked: {
                                        mpvVideo.seekRelative(-15)
                                        playerPage.beginSeekLoading()
                                        appViewModel.reportPlaybackProgress(Math.max(0, mpvVideo.position - 15), mpvVideo.duration, mpvVideo.paused)
                                        playerPage.revealControls()
                                    }
                                }

                                PlayerTransportButton {
                                    id: traditionalPauseButton
                                    primaryAction: true
                                    iconKind: mpvVideo.paused ? "play" : "pause"
                                    Accessible.name: mpvVideo.paused ? t("action.resume") : t("action.pause")
                                    onClicked: {
                                        mpvVideo.togglePause()
                                        appViewModel.reportPlaybackProgress(mpvVideo.position, mpvVideo.duration, mpvVideo.paused)
                                        playerPage.revealControls()
                                    }
                                }

                                PlayerTransportButton {
                                    id: traditionalForwardButton
                                    iconKind: "next"
                                    badgeText: "15"
                                    Accessible.name: t("action.forward15")
                                    onClicked: {
                                        mpvVideo.seekRelative(15)
                                        playerPage.beginSeekLoading()
                                        appViewModel.reportPlaybackProgress(mpvVideo.position + 15, mpvVideo.duration, mpvVideo.paused)
                                        playerPage.revealControls()
                                    }
                                }

                                Item { Layout.fillWidth: true }

                                PlayerChromeButton {
                                    id: traditionalSubtitleButton
                                    compact: true
                                    iconText: "CC"
                                    Accessible.name: t("player.subtitles")
                                    onClicked: {
                                        playerPage.openTrackMenu("subtitle", traditionalSubtitleButton)
                                        playerPage.revealControls()
                                    }
                                }

                                PlayerChromeButton {
                                    id: traditionalAudioButton
                                    compact: true
                                    iconText: "A"
                                    enabled: mpvVideo.audioTracks.count > 1
                                    Accessible.name: t("player.audio")
                                    onClicked: {
                                        playerPage.openTrackMenu("audio", traditionalAudioButton)
                                        playerPage.revealControls()
                                    }
                                }

                                PlayerChromeButton {
                                    id: traditionalSpeedButton
                                    compact: true
                                    iconText: playerPage.speedLabel(mpvVideo.speed)
                                    Accessible.name: t("player.currentSpeed").arg(playerPage.speedLabel(mpvVideo.speed))
                                    onClicked: {
                                        playerPage.openTrackMenu("speed", traditionalSpeedButton)
                                        playerPage.revealControls()
                                    }
                                }

                                PlayerChromeButton {
                                    id: traditionalInfoButton
                                    compact: true
                                    iconText: "i"
                                    Accessible.name: t("player.info")
                                    onClicked: {
                                        playerPage.closeIptvChannelList(false)
                                        playerPage.videoInfoVisible = !playerPage.videoInfoVisible
                                        playerPage.revealControls()
                                        Qt.callLater(playerPage.raiseChromeWindows)
                                    }
                                }

                                MutedText {
                                    text: t("player.volume")
                                    color: "#b3bdc9"
                                    font.pixelSize: 10
                                }

                                Slider {
                                    id: traditionalVolumeSlider
                                    Layout.preferredWidth: 62
                                    from: 0
                                    to: 100
                                    value: mpvVideo.volume
                                    onMoved: mpvVideo.setVolume(value)
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 7

                                MutedText {
                                    text: playerPage.formatTime(playerPage.displayedPlaybackPosition)
                                    color: "#d5dce6"
                                    font.pixelSize: 10
                                }

                                Slider {
                                    id: traditionalProgressSlider
                                    Layout.fillWidth: true
                                    implicitHeight: 16
                                    from: 0
                                    to: Math.max(1, mpvVideo.duration)
                                    value: playerPage.displayedPlaybackPosition
                                    onPressedChanged: {
                                        if (pressed) {
                                            playerPage.beginProgressSeek(value)
                                        } else {
                                            playerPage.commitProgressSeek()
                                        }
                                    }
                                    onMoved: playerPage.updateProgressSeek(value)
                                }

                                MutedText {
                                    text: "-" + playerPage.formatTime(Math.max(0, mpvVideo.duration - playerPage.displayedPlaybackPosition))
                                    color: "#d5dce6"
                                    font.pixelSize: 10
                                }
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
            visible: appViewModel.currentView === "player" && root.visible
                && !appViewModel.webDavAudioPlaybackActive
                && !playerPage.traditionalLayout
                && (playerPage.controlsVisible || playerPage.exitConfirmVisible)

            function syncChromeGeometry() {
                if (playerPage.width <= 0 || playerPage.height <= 0) {
                    return
                }
                var panelWidth = Math.min(1040, Math.max(1, playerPage.width - 48))
                var panelX = Math.max(24, (playerPage.width - panelWidth) / 2)
                var panelY = playerPage.height - playerPage.bottomChromeHeight - playerPage.bottomChromeMargin
                var origin = playerPage.mapToGlobal(panelX, panelY)
                x = Math.round(origin.x)
                y = Math.round(origin.y)
                width = Math.round(Math.min(panelWidth, playerPage.width - panelX * 2))
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
                    radius: 10
                    color: playerPage.chromePanelColor
                    border.color: "#44343b46"
                    clip: true

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
                    spacing: 10

                    PlayerChromeButton {
                        id: bottomExitButton
                        iconText: "X"
                        text: t("action.exitPlayback")
                        danger: true
                        Accessible.name: text
                        onClicked: playerPage.exitPlaybackImmediately()
                    }

                    BodyText {
                        Layout.fillWidth: true
                        text: playerPage.exitConfirmVisible
                            ? t("dialog.exitPlaybackPrompt") : appViewModel.selectedItemName
                        color: "#f4f7fb"
                        elide: Text.ElideRight
                        horizontalAlignment: Text.AlignHCenter
                    }

                    MutedText {
                        visible: !playerPage.exitConfirmVisible
                        text: playerPage.playbackMetricsText()
                        color: "#c7d0dd"
                        font.pixelSize: 11
                        horizontalAlignment: Text.AlignRight
                    }

                    PlayerChromeButton {
                        visible: playerPage.exitConfirmVisible
                        iconText: "X"
                        text: t("action.cancel")
                        Accessible.name: text
                        onClicked: playerPage.cancelExitPlayback()
                    }

                    PlayerChromeButton {
                        visible: playerPage.exitConfirmVisible
                        iconText: "OK"
                        text: t("action.exitPlayback")
                        danger: true
                        Accessible.name: text
                        onClicked: playerPage.confirmExitPlayback()
                    }

                    PlayerChromeButton {
                        visible: !playerPage.exitConfirmVisible
                        iconText: "[ ]"
                        text: playerPage.immersive ? t("action.exitFullscreen") : t("action.fullscreen")
                        Accessible.name: text
                        onClicked: playerPage.toggleFullscreen()
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    MutedText {
                        text: playerPage.formatTime(playerPage.displayedPlaybackPosition)
                        color: "#c7d0dd"
                    }

                    Slider {
                        id: progressSlider
                        Layout.fillWidth: true
                        from: 0
                        to: Math.max(1, mpvVideo.duration)
                        value: playerPage.displayedPlaybackPosition
                        onPressedChanged: {
                            if (pressed) {
                                playerPage.beginProgressSeek(value)
                            } else {
                                playerPage.commitProgressSeek()
                            }
                        }
                        onMoved: playerPage.updateProgressSeek(value)
                    }

                    MutedText {
                        text: playerPage.formatTime(mpvVideo.duration)
                        color: "#c7d0dd"
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    PlayerTransportButton {
                        iconKind: "previous"
                        badgeText: "15"
                        Accessible.name: t("action.rewind15")
                        onClicked: {
                            mpvVideo.seekRelative(-15)
                            playerPage.beginSeekLoading()
                            appViewModel.reportPlaybackProgress(Math.max(0, mpvVideo.position - 15), mpvVideo.duration, mpvVideo.paused)
                            playerPage.revealControls()
                        }
                    }

                    PlayerTransportButton {
                        primaryAction: true
                        iconKind: mpvVideo.paused ? "play" : "pause"
                        Accessible.name: mpvVideo.paused ? t("action.resume") : t("action.pause")
                        onClicked: {
                            mpvVideo.togglePause()
                            appViewModel.reportPlaybackProgress(mpvVideo.position, mpvVideo.duration, mpvVideo.paused)
                            playerPage.revealControls()
                        }
                    }

                    PlayerTransportButton {
                        iconKind: "next"
                        badgeText: "15"
                        Accessible.name: t("action.forward15")
                        onClicked: {
                            mpvVideo.seekRelative(15)
                            playerPage.beginSeekLoading()
                            appViewModel.reportPlaybackProgress(mpvVideo.position + 15, mpvVideo.duration, mpvVideo.paused)
                            playerPage.revealControls()
                        }
                    }

                    PlayerChromeButton {
                        id: speedButton
                        iconText: playerPage.speedLabel(mpvVideo.speed)
                        text: t("player.speed")
                        Accessible.name: t("player.currentSpeed").arg(playerPage.speedLabel(mpvVideo.speed))
                        onClicked: {
                            playerPage.openTrackMenu("speed", speedButton)
                            playerPage.revealControls()
                        }
                    }

                    PlayerChromeButton {
                        visible: appViewModel.iptvPlaybackActive
                        iconText: "CH"
                        text: t("iptv.playerChannels")
                        Accessible.name: text
                        onClicked: {
                            playerPage.openIptvChannelList()
                            playerPage.revealControls()
                        }
                    }

                    PlayerChromeButton {
                        id: subtitleTrackButton
                        iconText: "CC"
                        text: t("player.subtitles")
                        Accessible.name: text
                        onClicked: {
                            playerPage.openTrackMenu("subtitle", subtitleTrackButton)
                            playerPage.revealControls()
                        }
                    }

                    PlayerChromeButton {
                        id: audioTrackButton
                        iconText: "A"
                        text: t("player.audio")
                        enabled: mpvVideo.audioTracks.count > 1
                        Accessible.name: text
                        onClicked: {
                            playerPage.openTrackMenu("audio", audioTrackButton)
                            playerPage.revealControls()
                        }
                    }

                    PlayerChromeButton {
                        iconText: "i"
                        text: t("player.info")
                        Accessible.name: text
                        onClicked: {
                            playerPage.closeIptvChannelList(false)
                            playerPage.videoInfoVisible = !playerPage.videoInfoVisible
                            playerPage.revealControls()
                            Qt.callLater(playerPage.raiseChromeWindows)
                        }
                    }

                    PlayerChromeButton {
                        iconText: "C"
                        text: t("player.cacheShort") + " " + playerPage.cacheDurationText(mpvVideo.cacheDurationSeconds)
                        Accessible.name: text
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

    component LocalMediaPage: Item {
        id: localMediaPage

        ColumnLayout {
            anchors.fill: parent
            spacing: 14

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                SectionHeader {
                    Layout.fillWidth: true
                    title: appViewModel.localMediaDirectoryOpen
                        ? appViewModel.localMediaRootName : t("local.foldersTitle")
                    subtitle: appViewModel.localMediaDirectoryOpen
                        ? appViewModel.localMediaCurrentPath : t("local.foldersSubtitle")
                }

                ModernButton {
                    text: appViewModel.localMediaDirectoryOpen
                        ? t("local.back") : t("local.addFolder")
                    onClicked: {
                        if (appViewModel.localMediaDirectoryOpen) {
                            appViewModel.localMediaBack()
                        } else {
                            appViewModel.chooseLocalMediaRoot()
                        }
                    }
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                ListView {
                    id: localRootList
                    anchors.fill: parent
                    visible: !appViewModel.localMediaDirectoryOpen
                    enabled: !appViewModel.localMediaLoading
                    model: visible ? appViewModel.localMediaRoots : null
                    spacing: 10
                    clip: true

                    delegate: Rectangle {
                        width: localRootList.width
                        height: 82
                        radius: 10
                        color: rootMouse.containsMouse ? theme.elevatedHover : theme.elevated
                        border.color: model.available ? theme.border : root.withAlpha(theme.warning, 0.62)

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 14
                            anchors.rightMargin: 14
                            spacing: 12

                            Item {
                                Layout.fillWidth: true
                                Layout.fillHeight: true

                                MouseArea {
                                    id: rootMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    enabled: model.available
                                    onClicked: appViewModel.openLocalMediaRoot(index)
                                }

                                RowLayout {
                                    anchors.fill: parent
                                    spacing: 13

                                    ServiceTypeIcon {
                                        Layout.preferredWidth: 46
                                        Layout.preferredHeight: 46
                                        serviceType: "Local"
                                    }

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 4

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
                                            text: model.path
                                            elide: Text.ElideMiddle
                                        }
                                    }

                                    ServiceStatusChip {
                                        text: model.available ? t("local.available") : t("local.unavailable")
                                        accentColor: model.available ? theme.success : theme.warning
                                    }
                                }
                            }

                            ModernButton {
                                text: t("local.remove")
                                onClicked: appViewModel.deleteLocalMediaRoot(index)
                            }
                        }
                    }
                }

                ListView {
                    id: localItemList
                    anchors.fill: parent
                    visible: appViewModel.localMediaDirectoryOpen
                    enabled: !appViewModel.localMediaLoading
                    opacity: appViewModel.localMediaLoading ? 0.32 : 1
                    model: visible ? appViewModel.localMediaItems : null
                    spacing: 8
                    clip: true

                    delegate: Rectangle {
                        width: localItemList.width
                        height: 70
                        radius: 9
                        color: itemMouse.containsMouse ? theme.elevatedHover : theme.elevated
                        border.color: itemMouse.containsMouse
                            ? root.withAlpha(theme.primary, 0.66) : theme.border

                        MouseArea {
                            id: itemMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: appViewModel.openLocalMediaItem(index)
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 15
                            anchors.rightMargin: 15
                            spacing: 14

                            Rectangle {
                                Layout.preferredWidth: 40
                                Layout.preferredHeight: 40
                                radius: 10
                                color: root.withAlpha(theme.primary, darkTheme ? 0.20 : 0.11)
                                border.color: root.withAlpha(theme.primary, 0.38)

                                Label {
                                    anchors.centerIn: parent
                                    text: model.directory ? "▰" : "▶"
                                    color: theme.primary
                                    font.pixelSize: model.directory ? 20 : 16
                                    font.bold: true
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 4

                                Label {
                                    Layout.fillWidth: true
                                    text: model.name
                                    color: theme.text
                                    font.pixelSize: 14
                                    font.bold: true
                                    elide: Text.ElideRight
                                }

                                MutedText {
                                    Layout.fillWidth: true
                                    text: model.directory
                                        ? t("local.folder")
                                        : t("local.video") + "  ·  " + root.formatBytes(model.bytes)
                                            + "  ·  " + Qt.formatDateTime(model.lastModified, "yyyy-MM-dd HH:mm")
                                    elide: Text.ElideRight
                                }
                            }

                            Label {
                                text: model.directory ? "›" : "▶"
                                color: theme.primary
                                font.pixelSize: 20
                            }
                        }

                        Behavior on color { ColorAnimation { duration: 120 } }
                    }

                    Behavior on opacity { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                }

                ColumnLayout {
                    anchors.centerIn: parent
                    visible: !appViewModel.localMediaLoading
                        && (appViewModel.localMediaDirectoryOpen
                            ? appViewModel.localMediaItems.count === 0
                            : appViewModel.localMediaRoots.count === 0)
                    spacing: 10

                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: appViewModel.localMediaDirectoryOpen
                            ? t("local.noVideos") : t("local.noFolders")
                        color: theme.text
                        font.pixelSize: 20
                        font.bold: true
                    }

                    MutedText {
                        Layout.alignment: Qt.AlignHCenter
                        text: appViewModel.localMediaDirectoryOpen
                            ? t("local.noVideosHint") : t("local.noFoldersHint")
                    }

                    ModernButton {
                        visible: !appViewModel.localMediaDirectoryOpen
                        Layout.alignment: Qt.AlignHCenter
                        text: t("local.addFolder")
                        onClicked: appViewModel.chooseLocalMediaRoot()
                    }
                }

                Rectangle {
                    anchors.fill: parent
                    visible: appViewModel.localMediaLoading
                    color: root.darkTheme ? "#d90f1217" : "#ddf5f7fb"
                    z: 10

                    MouseArea { anchors.fill: parent }

                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: 10

                        BusyIndicator {
                            Layout.alignment: Qt.AlignHCenter
                            running: parent.parent.visible
                            implicitWidth: 44
                            implicitHeight: 44
                        }

                        Label {
                            Layout.alignment: Qt.AlignHCenter
                            text: t("local.loading")
                            color: theme.text
                            font.pixelSize: 18
                            font.bold: true
                        }
                    }
                }
            }
        }
    }

    component LinkPlaybackPage: Flickable {
        id: linkPlaybackFlick
        contentWidth: width
        contentHeight: linkPlaybackColumn.implicitHeight
        clip: true

        onVisibleChanged: {
            if (visible) {
                Qt.callLater(linkAddressField.forceActiveFocus)
            }
        }

        ColumnLayout {
            id: linkPlaybackColumn
            width: linkPlaybackFlick.width
            spacing: 18

            SectionHeader {
                Layout.fillWidth: true
                title: t("link.formTitle")
                subtitle: t("link.formSubtitle")
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.maximumWidth: 760
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredHeight: linkForm.implicitHeight + 48
                radius: 16
                color: theme.elevated
                border.color: theme.border

                ColumnLayout {
                    id: linkForm
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 24
                    spacing: 14

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 14

                        ServiceTypeIcon {
                            Layout.preferredWidth: 54
                            Layout.preferredHeight: 54
                            serviceType: "Link"
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3

                            Label {
                                Layout.fillWidth: true
                                text: t("link.title")
                                color: theme.text
                                font.pixelSize: 19
                                font.bold: true
                            }

                            MutedText {
                                Layout.fillWidth: true
                                text: t("link.protocols")
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: t("link.address")
                        color: theme.text
                        font.pixelSize: 13
                        font.bold: true
                    }

                    ModernTextField {
                        id: linkAddressField
                        Layout.fillWidth: true
                        placeholderText: t("link.placeholder")
                        text: appViewModel.linkPlaybackAddress
                        selectByMouse: true
                        onTextChanged: appViewModel.linkPlaybackAddress = text
                        onAccepted: appViewModel.playLink()
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        MutedText {
                            Layout.fillWidth: true
                            text: t("link.historyStorage")
                            wrapMode: Text.WordWrap
                        }

                        ModernButton {
                            text: t("link.playNow")
                            enabled: linkAddressField.text.trim().length > 0
                            onClicked: appViewModel.playLink()
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: linkHint.implicitHeight + 22
                        radius: 10
                        color: root.withAlpha(root.serviceAccentColor("Link"), darkTheme ? 0.12 : 0.08)
                        border.color: root.withAlpha(root.serviceAccentColor("Link"), 0.34)

                        MutedText {
                            id: linkHint
                            anchors.fill: parent
                            anchors.margins: 11
                            text: t("link.supportedHint")
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }

            SectionHeader {
                Layout.fillWidth: true
                Layout.maximumWidth: 900
                Layout.alignment: Qt.AlignHCenter
                title: t("link.historyTitle")
                subtitle: t("link.historySubtitle")
            }

            ListView {
                id: linkHistoryList
                Layout.fillWidth: true
                Layout.maximumWidth: 900
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredHeight: contentHeight
                visible: appViewModel.linkPlaybackHistory.count > 0
                interactive: false
                spacing: 8
                clip: false
                model: appViewModel.linkPlaybackHistory
                section.property: "playedDate"
                section.criteria: ViewSection.FullString
                section.delegate: Item {
                    required property string section
                    width: linkHistoryList.width
                    height: 36

                    Label {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        text: section
                        color: theme.muted
                        font.pixelSize: 13
                        font.bold: true
                        elide: Text.ElideRight
                    }
                }

                delegate: Rectangle {
                    required property string recordId
                    required property string displayName
                    required property string displayAddress
                    required property string playedTime
                    required property bool privacyMode

                    width: linkHistoryList.width
                    height: 84
                    radius: 12
                    color: linkHistoryMouse.containsMouse ? theme.elevatedHover : theme.elevated
                    border.color: linkHistoryMouse.containsMouse ? theme.primary : theme.border

                    MouseArea {
                        id: linkHistoryMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: appViewModel.playLinkHistory(recordId)
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: 12

                        ServiceTypeIcon {
                            Layout.preferredWidth: 44
                            Layout.preferredHeight: 44
                            serviceType: "Link"
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 7

                                Label {
                                    Layout.fillWidth: true
                                    text: displayName
                                    color: theme.text
                                    font.pixelSize: 15
                                    font.bold: true
                                    elide: Text.ElideRight
                                }

                                Label {
                                    visible: privacyMode
                                    text: t("globalHistory.privateBadge")
                                    color: theme.primary
                                    font.pixelSize: 10
                                    font.bold: true
                                }
                            }

                            MutedText {
                                Layout.fillWidth: true
                                text: displayAddress
                                elide: Text.ElideMiddle
                            }
                        }

                        MutedText {
                            text: playedTime
                            font.pixelSize: 12
                        }

                        ModernButton {
                            text: t("link.playAgain")
                            onClicked: appViewModel.playLinkHistory(recordId)
                        }

                        ModernButton {
                            text: t("link.deleteHistory")
                            danger: true
                            onClicked: appViewModel.deleteLinkPlaybackHistory(recordId)
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.maximumWidth: 900
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredHeight: 128
                visible: appViewModel.linkPlaybackHistory.count === 0
                radius: 12
                color: theme.elevated
                border.color: theme.border

                ColumnLayout {
                    anchors.centerIn: parent
                    spacing: 8

                    ServiceTypeIcon {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.preferredWidth: 44
                        Layout.preferredHeight: 44
                        serviceType: "Link"
                    }

                    MutedText {
                        Layout.alignment: Qt.AlignHCenter
                        text: t("link.historyEmpty")
                    }
                }
            }

            Item { Layout.preferredHeight: 8 }
        }
    }

    component GlobalHistoryPage: Flickable {
        id: globalHistoryFlick
        contentWidth: width
        contentHeight: globalHistoryColumn.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollIndicator.vertical: ScrollIndicator {}

        property var sourceFilters: [
            { value: "All", labelKey: "globalHistory.filterAll" },
            { value: "Emby", labelKey: "globalHistory.sourceEmby" },
            { value: "Jellyfin", labelKey: "globalHistory.sourceJellyfin" },
            { value: "WebDAV", labelKey: "globalHistory.sourceWebDav" },
            { value: "IPTV", labelKey: "globalHistory.sourceIptv" },
            { value: "Local", labelKey: "globalHistory.sourceLocal" },
            { value: "Link", labelKey: "globalHistory.sourceLink" }
        ]

        function sourceLabel(sourceType) {
            switch (String(sourceType).toLowerCase()) {
            case "emby": return t("globalHistory.sourceEmby")
            case "jellyfin": return t("globalHistory.sourceJellyfin")
            case "webdav": return t("globalHistory.sourceWebDav")
            case "iptv": return t("globalHistory.sourceIptv")
            case "local": return t("globalHistory.sourceLocal")
            case "link": return t("globalHistory.sourceLink")
            default: return sourceType
            }
        }

        ColumnLayout {
            id: globalHistoryColumn
            width: globalHistoryFlick.width
            spacing: 16

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                SectionHeader {
                    Layout.fillWidth: true
                    title: t("globalHistory.recentTitle")
                    subtitle: t("globalHistory.recentSubtitle")
                }

                IconButton {
                    text: "↻"
                    font.pixelSize: 20
                    enabled: !appViewModel.globalHistoryLoading
                    ToolTip.visible: hovered
                    ToolTip.text: t("action.refresh")
                    onClicked: appViewModel.refreshGlobalHistory()
                }
            }

            Flickable {
                id: globalHistoryFilterFlick
                Layout.fillWidth: true
                Layout.preferredHeight: 40
                contentWidth: globalHistoryFilters.implicitWidth
                contentHeight: height
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                flickableDirection: Flickable.HorizontalFlick

                Row {
                    id: globalHistoryFilters
                    height: parent.height
                    spacing: 7

                    Repeater {
                        model: globalHistoryFlick.sourceFilters

                        delegate: Button {
                            id: historyFilterButton
                            required property var modelData
                            readonly property bool selected: appViewModel.globalHistoryFilter.toLowerCase()
                                === String(modelData.value).toLowerCase()
                            height: 36
                            width: Math.max(68, historyFilterLabel.implicitWidth + 26)
                            leftPadding: 13
                            rightPadding: 13
                            onClicked: appViewModel.globalHistoryFilter = modelData.value

                            contentItem: Label {
                                id: historyFilterLabel
                                text: t(historyFilterButton.modelData.labelKey)
                                color: historyFilterButton.selected ? "#ffffff" : theme.text
                                font.pixelSize: 13
                                font.bold: historyFilterButton.selected
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }

                            background: Rectangle {
                                radius: 8
                                color: historyFilterButton.selected
                                    ? root.serviceAccentColor("History")
                                    : historyFilterButton.hovered ? theme.elevatedHover : theme.elevated
                                border.color: historyFilterButton.selected
                                    ? root.serviceAccentColor("History")
                                    : historyFilterButton.hovered ? root.withAlpha(root.serviceAccentColor("History"), 0.72)
                                    : theme.border
                            }
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 24
                spacing: 8

                Label {
                    text: t("globalHistory.recordCount").arg(appViewModel.globalPlaybackHistory.count)
                    color: theme.muted
                    font.pixelSize: 13
                    font.bold: true
                }

                Rectangle {
                    visible: appViewModel.privacyMode
                    Layout.preferredWidth: globalHistoryPrivacyLabel.implicitWidth + 16
                    Layout.preferredHeight: 22
                    radius: 7
                    color: root.withAlpha(theme.primary, darkTheme ? 0.20 : 0.11)
                    border.color: root.withAlpha(theme.primary, 0.48)

                    Label {
                        id: globalHistoryPrivacyLabel
                        anchors.centerIn: parent
                        text: t("globalHistory.privateIncluded")
                        color: theme.primary
                        font.pixelSize: 10
                        font.bold: true
                    }
                }

                Item { Layout.fillWidth: true }

                BusyIndicator {
                    visible: appViewModel.globalHistoryLoading && appViewModel.globalPlaybackHistory.count > 0
                    running: visible
                    implicitWidth: 22
                    implicitHeight: 22
                }
            }

            ListView {
                id: globalHistoryList
                Layout.fillWidth: true
                Layout.preferredHeight: contentHeight
                visible: appViewModel.globalPlaybackHistory.count > 0
                interactive: false
                spacing: 9
                clip: false
                model: appViewModel.globalPlaybackHistory
                section.property: "playedDate"
                section.criteria: ViewSection.FullString
                section.delegate: Item {
                    required property string section
                    width: globalHistoryList.width
                    height: 38

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

                delegate: Rectangle {
                    id: globalHistoryRow
                    required property string recordId
                    required property string sourceType
                    required property string serviceName
                    required property string title
                    required property string subtitle
                    required property string displayTarget
                    required property string playedTime
                    required property real positionSeconds
                    required property real durationSeconds
                    required property real progress
                    required property bool completed
                    required property bool privacyMode
                    required property bool available
                    readonly property color accentColor: root.serviceAccentColor(sourceType)

                    width: globalHistoryList.width
                    height: 126
                    radius: 8
                    color: globalHistoryMouse.containsMouse ? theme.elevatedHover : theme.elevated
                    border.color: !available ? root.withAlpha(theme.danger, 0.50)
                        : globalHistoryMouse.containsMouse ? root.withAlpha(accentColor, 0.78) : theme.border
                    opacity: available ? 1.0 : 0.76

                    Behavior on color { ColorAnimation { duration: 120 } }
                    Behavior on border.color { ColorAnimation { duration: 120 } }

                    MouseArea {
                        id: globalHistoryMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: globalHistoryRow.available
                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: appViewModel.playGlobalHistory(globalHistoryRow.recordId)
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: 13

                        ServiceTypeIcon {
                            Layout.preferredWidth: 48
                            Layout.preferredHeight: 48
                            Layout.alignment: Qt.AlignTop
                            serviceType: globalHistoryRow.sourceType
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            spacing: 4

                            Label {
                                Layout.fillWidth: true
                                text: globalHistoryRow.title
                                color: theme.text
                                font.pixelSize: 16
                                font.bold: true
                                elide: Text.ElideRight
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 7

                                Rectangle {
                                    Layout.preferredWidth: globalHistorySourceLabel.implicitWidth + 14
                                    Layout.preferredHeight: 21
                                    radius: 7
                                    color: root.withAlpha(globalHistoryRow.accentColor, darkTheme ? 0.20 : 0.11)
                                    border.color: root.withAlpha(globalHistoryRow.accentColor, 0.46)

                                    Label {
                                        id: globalHistorySourceLabel
                                        anchors.centerIn: parent
                                        text: globalHistoryFlick.sourceLabel(globalHistoryRow.sourceType)
                                        color: globalHistoryRow.accentColor
                                        font.pixelSize: 10
                                        font.bold: true
                                    }
                                }

                                MutedText {
                                    Layout.fillWidth: true
                                    text: globalHistoryRow.subtitle.length > 0 && globalHistoryRow.serviceName.length > 0
                                        ? globalHistoryRow.subtitle + " · " + globalHistoryRow.serviceName
                                        : globalHistoryRow.subtitle.length > 0 ? globalHistoryRow.subtitle : globalHistoryRow.serviceName
                                    elide: Text.ElideRight
                                }

                                Label {
                                    visible: globalHistoryRow.privacyMode
                                    text: t("globalHistory.privateBadge")
                                    color: theme.primary
                                    font.pixelSize: 10
                                    font.bold: true
                                }

                                Label {
                                    visible: !globalHistoryRow.available
                                    text: t("globalHistory.unavailable")
                                    color: theme.danger
                                    font.pixelSize: 10
                                    font.bold: true
                                }
                            }

                            MutedText {
                                Layout.fillWidth: true
                                text: globalHistoryRow.displayTarget
                                font.pixelSize: 12
                                elide: Text.ElideMiddle
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 20
                                spacing: 10

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 4
                                    radius: 2
                                    color: theme.border
                                    visible: globalHistoryRow.durationSeconds > 0

                                    Rectangle {
                                        width: parent.width * globalHistoryRow.progress
                                        height: parent.height
                                        radius: parent.radius
                                        color: globalHistoryRow.accentColor
                                    }
                                }

                                Label {
                                    text: globalHistoryRow.completed
                                        ? t("globalHistory.completed")
                                        : globalHistoryRow.positionSeconds > 0
                                            ? t("globalHistory.resumeAt").arg(root.formatPlaybackTime(globalHistoryRow.positionSeconds))
                                            : t("globalHistory.started")
                                    color: globalHistoryRow.completed ? theme.success : theme.muted
                                    font.pixelSize: 11
                                    font.bold: globalHistoryRow.completed
                                }

                                MutedText {
                                    visible: globalHistoryRow.durationSeconds > 0
                                    text: root.formatPlaybackTime(globalHistoryRow.positionSeconds)
                                        + " / " + root.formatPlaybackTime(globalHistoryRow.durationSeconds)
                                    font.pixelSize: 11
                                }
                            }
                        }

                        ColumnLayout {
                            Layout.preferredWidth: 92
                            Layout.fillHeight: true
                            spacing: 7

                            MutedText {
                                Layout.alignment: Qt.AlignRight
                                text: globalHistoryRow.playedTime
                                font.pixelSize: 11
                            }

                            Item { Layout.fillHeight: true }

                            ModernButton {
                                Layout.fillWidth: true
                                text: globalHistoryRow.available
                                    ? (globalHistoryRow.positionSeconds > 0 && !globalHistoryRow.completed
                                        ? t("action.continue") : t("action.play"))
                                    : t("globalHistory.unavailable")
                                enabled: globalHistoryRow.available
                                onClicked: appViewModel.playGlobalHistory(globalHistoryRow.recordId)
                            }

                            IconButton {
                                Layout.alignment: Qt.AlignRight
                                implicitWidth: 34
                                implicitHeight: 30
                                text: "×"
                                danger: true
                                ToolTip.visible: hovered
                                ToolTip.text: t("action.delete")
                                onClicked: appViewModel.deleteGlobalHistory(globalHistoryRow.recordId)
                            }
                        }
                    }
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: 190
                visible: appViewModel.globalPlaybackHistory.count === 0 && !appViewModel.globalHistoryLoading

                ColumnLayout {
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 48, 440)
                    spacing: 9

                    ServiceTypeIcon {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.preferredWidth: 52
                        Layout.preferredHeight: 52
                        serviceType: "History"
                    }

                    Label {
                        Layout.fillWidth: true
                        text: t("globalHistory.empty")
                        color: theme.text
                        font.pixelSize: 17
                        font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                    }

                    MutedText {
                        Layout.fillWidth: true
                        text: t("globalHistory.emptyHint")
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                    }
                }
            }

            PageLoadingPanel {
                Layout.alignment: Qt.AlignHCenter
                visible: appViewModel.globalHistoryLoading && appViewModel.globalPlaybackHistory.count === 0
                title: t("globalHistory.loading")
                subtitle: t("globalHistory.loadingHint")
            }

            ModernButton {
                Layout.alignment: Qt.AlignHCenter
                visible: appViewModel.globalHistoryHasMore
                enabled: !appViewModel.globalHistoryLoading
                text: t("globalHistory.loadMore")
                onClicked: appViewModel.loadMoreGlobalHistory()
            }

            Item { Layout.preferredHeight: 8 }
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

        ColumnLayout {
            id: webDavColumn
            anchors.fill: parent
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
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                Label {
                    text: t("webdav.displayMode")
                    color: theme.muted
                    font.pixelSize: 12
                    font.bold: true
                }

                WebDavDisplayModeSwitch {}

                MutedText {
                    visible: (appViewModel.webDavDisplayMode === "video" || appViewModel.webDavDisplayMode === "audio")
                        && webDavPage.width >= 1080
                    text: appViewModel.webDavDisplayMode === "audio"
                        ? t("webdav.audioModeHint") : t("webdav.videoModeHint")
                }

                Item {
                    Layout.fillWidth: true
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                MutedText {
                    Layout.fillWidth: true
                    visible: appViewModel.webDavTsslStatus.length > 0
                    text: appViewModel.webDavTsslStatus
                    elide: Text.ElideRight
                }

                ModernButton {
                    text: t("webdav.tsslRestore")
                    onClicked: appViewModel.restoreTssl()
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
                property bool videoMode: appViewModel.webDavDisplayMode === "video"
                property bool audioMode: appViewModel.webDavDisplayMode === "audio"
                property int gridColumns: Math.max(1, Math.floor(width / 226))
                property real gridCellWidth: width / gridColumns
                property real gridCellHeight: 226
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                ListView {
                    anchors.fill: parent
                    visible: !webDavListArea.videoMode && !webDavListArea.audioMode
                    enabled: !appViewModel.loading
                    opacity: appViewModel.loading ? 0.34 : 1
                    spacing: 10
                    model: visible ? appViewModel.webDavItems : null
                    delegate: WebDavFileRow {
                        width: ListView.view.width
                        title: model.name
                        subtitle: model.directory
                            ? t("webdav.folder")
                            : (model.contentType.length > 0 ? model.contentType + "  " : "")
                                + (model.bytes >= 0 ? root.formatBytes(model.bytes) : "")
                        directory: model.directory
                        playable: model.playable
                        encryptedHls: model.encryptedHls
                        onActivated: appViewModel.openWebDavItem(index)
                        onDownloadRequested: appViewModel.downloadWebDavItem(index)
                        onExportTsslRequested: appViewModel.exportWebDavTssl(index)
                    }
                    Behavior on opacity { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                }

                GridView {
                    id: webDavVideoGrid
                    anchors.fill: parent
                    visible: webDavListArea.videoMode
                    enabled: !appViewModel.loading
                    opacity: appViewModel.loading ? 0.34 : 1
                    cellWidth: webDavListArea.gridCellWidth
                    cellHeight: webDavListArea.gridCellHeight
                    model: visible ? appViewModel.webDavItems : null
                    delegate: WebDavMediaCard {
                        width: webDavVideoGrid.cellWidth - 12
                        height: 214
                        title: model.name
                        contentType: model.contentType
                        bytes: model.bytes
                        directory: model.directory
                        encryptedHls: model.encryptedHls
                        onActivated: appViewModel.openWebDavItem(index)
                        onDownloadRequested: appViewModel.downloadWebDavItem(index)
                        onExportTsslRequested: appViewModel.exportWebDavTssl(index)
                    }
                    Behavior on opacity { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                }

                ListView {
                    id: webDavAudioList
                    anchors.fill: parent
                    visible: webDavListArea.audioMode
                    enabled: !appViewModel.loading
                    opacity: appViewModel.loading ? 0.34 : 1
                    spacing: 8
                    clip: true
                    model: visible ? appViewModel.webDavItems : null
                    delegate: Rectangle {
                        width: webDavAudioList.width
                        height: 70
                        radius: 8
                        color: index === appViewModel.webDavAudioCurrentIndex
                            ? root.withAlpha(theme.primary, darkTheme ? 0.24 : 0.12)
                            : (audioMouse.containsMouse ? theme.elevatedHover : theme.elevated)
                        border.color: index === appViewModel.webDavAudioCurrentIndex
                            ? root.withAlpha(theme.primary, 0.72)
                            : theme.border

                        MouseArea {
                            id: audioMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: appViewModel.openWebDavItem(index)
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 16
                            anchors.rightMargin: 16
                            spacing: 14

                            Rectangle {
                                Layout.preferredWidth: 38
                                Layout.preferredHeight: 38
                                radius: 19
                                color: root.withAlpha(theme.primary, index === appViewModel.webDavAudioCurrentIndex ? 0.9 : 0.16)

                                Label {
                                    anchors.centerIn: parent
                                    text: index === appViewModel.webDavAudioCurrentIndex ? "\u266B" : "\u266A"
                                    color: index === appViewModel.webDavAudioCurrentIndex ? "#ffffff" : theme.primary
                                    font.pixelSize: 18
                                    font.bold: true
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 3

                                Label {
                                    Layout.fillWidth: true
                                    text: model.name
                                    color: theme.text
                                    font.pixelSize: 14
                                    font.bold: index === appViewModel.webDavAudioCurrentIndex
                                    elide: Text.ElideRight
                                }

                                MutedText {
                                    Layout.fillWidth: true
                                    text: model.contentType.length > 0
                                        ? model.contentType
                                        : (model.bytes >= 0 ? root.formatBytes(model.bytes) : t("webdav.audio"))
                                    elide: Text.ElideRight
                                }
                            }

                            Label {
                                visible: index === appViewModel.webDavAudioCurrentIndex
                                text: "\u25B6"
                                color: theme.primary
                                font.pixelSize: 16
                            }
                        }
                    }
                    Behavior on opacity { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                }

                MutedText {
                    anchors.centerIn: parent
                    visible: appViewModel.webDavItems.count === 0 && !appViewModel.loading
                    text: webDavListArea.audioMode
                        ? t("webdav.audioEmpty")
                        : webDavListArea.videoMode ? t("webdav.videoEmpty") : t("webdav.empty")
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
                        text: serviceType.toLowerCase() === "link"
                            ? t("link.title")
                            : serviceName.length > 0 ? serviceName : t("history.service")
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

    component M3u8sManagerPage: Flickable {
        id: m3u8sFlick
        readonly property color accentColor: root.serviceAccentColor("M3u8s")
        contentWidth: width
        contentHeight: m3u8sColumn.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        function phaseText(phase) {
            var key = "m3u8s.phase." + phase
            var translated = t(key)
            return translated === key ? t("m3u8s.processingStatus") : translated
        }

        ColumnLayout {
            id: m3u8sColumn
            width: m3u8sFlick.width
            spacing: 18

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: packageCreatorContent.implicitHeight + 34
                radius: 8
                color: theme.surface
                border.color: appViewModel.m3u8sPackaging
                    ? root.withAlpha(m3u8sFlick.accentColor, 0.82) : theme.border

                ColumnLayout {
                    id: packageCreatorContent
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 17
                    spacing: 14

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 14

                        ServiceTypeIcon {
                            Layout.preferredWidth: 52
                            Layout.preferredHeight: 52
                            Layout.alignment: Qt.AlignTop
                            serviceType: "M3u8s"
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Label {
                                Layout.fillWidth: true
                                text: t("m3u8s.createTitle")
                                color: theme.text
                                font.pixelSize: 19
                                font.bold: true
                                elide: Text.ElideRight
                            }

                            MutedText {
                                Layout.fillWidth: true
                                text: t("m3u8s.createSubtitle")
                                wrapMode: Text.WordWrap
                            }
                        }

                        Rectangle {
                            Layout.preferredWidth: ffmpegStatusLabel.implicitWidth + 26
                            Layout.preferredHeight: 30
                            radius: 8
                            color: root.withAlpha(appViewModel.m3u8sFfmpegAvailable
                                ? theme.success : theme.danger, darkTheme ? 0.16 : 0.09)
                            border.color: root.withAlpha(appViewModel.m3u8sFfmpegAvailable
                                ? theme.success : theme.danger, 0.52)

                            Label {
                                id: ffmpegStatusLabel
                                anchors.centerIn: parent
                                text: appViewModel.m3u8sFfmpegAvailable
                                    ? t("m3u8s.ffmpegReady") : t("m3u8s.ffmpegMissing")
                                color: appViewModel.m3u8sFfmpegAvailable ? theme.success : theme.danger
                                font.pixelSize: 11
                                font.bold: true
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        Label {
                            Layout.preferredWidth: 112
                            text: t("m3u8s.outputDirectory")
                            color: theme.text
                            font.pixelSize: 13
                            font.bold: true
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Layout.preferredHeight: 38
                            radius: 8
                            color: theme.input
                            border.color: theme.border

                            Label {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 12
                                text: appViewModel.m3u8sOutputDirectory
                                color: theme.text
                                font.pixelSize: 13
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideMiddle
                            }
                        }

                        ModernButton {
                            enabled: !appViewModel.m3u8sPackaging
                            text: t("m3u8s.chooseFolder")
                            onClicked: appViewModel.chooseM3u8sOutputDirectory()
                        }

                        ModernButton {
                            enabled: appViewModel.m3u8sOutputDirectory.length > 0
                            text: t("m3u8s.openOutput")
                            onClicked: appViewModel.openM3u8sConfiguredOutputDirectory()
                        }
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: width < 760 ? 1 : 3
                        columnSpacing: 12
                        rowSpacing: 10

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Layout.preferredWidth: 1
                            spacing: 6

                            Label {
                                text: t("m3u8s.videoEncoding")
                                color: theme.text
                                font.pixelSize: 12
                                font.bold: true
                            }

                            ModernComboBox {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                enabled: !appViewModel.m3u8sPackaging
                                textRole: "label"
                                valueRole: "value"
                                model: [
                                    { label: t("m3u8s.encodingCopy"), value: "copy" },
                                    { label: t("m3u8s.encodingH264"), value: "h264" },
                                    { label: t("m3u8s.encodingH265"), value: "h265" }
                                ]
                                currentIndex: appViewModel.m3u8sVideoEncoding === "copy" ? 0
                                    : appViewModel.m3u8sVideoEncoding === "h265" ? 2 : 1
                                onActivated: appViewModel.m3u8sVideoEncoding = model[index].value
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Layout.preferredWidth: 1
                            spacing: 6

                            Label {
                                text: t("m3u8s.audioEncoding")
                                color: theme.text
                                font.pixelSize: 12
                                font.bold: true
                            }

                            ModernComboBox {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                enabled: !appViewModel.m3u8sPackaging
                                textRole: "label"
                                valueRole: "value"
                                model: [
                                    { label: t("m3u8s.encodingCopy"), value: "copy" },
                                    { label: t("m3u8s.audioAac"), value: "aac" }
                                ]
                                currentIndex: appViewModel.m3u8sAudioEncoding === "copy" ? 0 : 1
                                onActivated: appViewModel.m3u8sAudioEncoding = model[index].value
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Layout.preferredWidth: 1
                            spacing: 6

                            Label {
                                text: t("m3u8s.videoQuality")
                                color: theme.text
                                font.pixelSize: 12
                                font.bold: true
                            }

                            ModernComboBox {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                enabled: !appViewModel.m3u8sPackaging
                                    && appViewModel.m3u8sVideoEncoding !== "copy"
                                opacity: enabled ? 1.0 : 0.5
                                textRole: "label"
                                valueRole: "value"
                                model: [
                                    { label: t("m3u8s.qualityHigh"), value: "high" },
                                    { label: t("m3u8s.qualityBalanced"), value: "balanced" },
                                    { label: t("m3u8s.qualityCompact"), value: "compact" }
                                ]
                                currentIndex: appViewModel.m3u8sVideoQuality === "high" ? 0
                                    : appViewModel.m3u8sVideoQuality === "compact" ? 2 : 1
                                onActivated: appViewModel.m3u8sVideoQuality = model[index].value
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        Label {
                            text: t("m3u8s.segmentDuration")
                            color: theme.text
                            font.pixelSize: 13
                            font.bold: true
                        }

                        ModernSpinBox {
                            from: 2
                            to: 30
                            stepSize: 1
                            value: appViewModel.m3u8sSegmentDuration
                            enabled: !appViewModel.m3u8sPackaging
                            onValueModified: appViewModel.m3u8sSegmentDuration = value
                            textFromValue: function(value, locale) {
                                return t("m3u8s.seconds").arg(value)
                            }
                            valueFromText: function(text, locale) {
                                var parsed = parseInt(text)
                                return isNaN(parsed) ? appViewModel.m3u8sSegmentDuration : parsed
                            }
                        }

                        Item { Layout.fillWidth: true }

                        ModernButton {
                            visible: appViewModel.m3u8sLastOutputDirectory.length > 0
                            text: t("m3u8s.openLastOutput")
                            onClicked: appViewModel.openM3u8sOutputDirectory()
                        }

                        ModernButton {
                            visible: appViewModel.m3u8sPackaging
                            text: t("action.cancel")
                            danger: true
                            onClicked: appViewModel.cancelM3u8sPackaging()
                        }

                        ModernButton {
                            visible: !appViewModel.m3u8sPackaging
                            enabled: appViewModel.m3u8sFfmpegAvailable
                                && appViewModel.m3u8sOutputDirectory.length > 0
                            text: t("m3u8s.createAction")
                            onClicked: appViewModel.chooseM3u8sVideo()
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: appViewModel.m3u8sPackaging
                        spacing: 7

                        RowLayout {
                            Layout.fillWidth: true

                            Label {
                                Layout.fillWidth: true
                                text: m3u8sFlick.phaseText(appViewModel.m3u8sPackagingPhase)
                                color: m3u8sFlick.accentColor
                                font.pixelSize: 12
                                font.bold: true
                                elide: Text.ElideRight
                            }

                            Label {
                                text: Math.round(appViewModel.m3u8sPackagingProgress * 100) + "%"
                                color: theme.text
                                font.pixelSize: 12
                                font.bold: true
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 7
                            radius: 3
                            color: theme.input
                            clip: true

                            Rectangle {
                                width: parent.width * Math.max(0, Math.min(1, appViewModel.m3u8sPackagingProgress))
                                height: parent.height
                                radius: parent.radius
                                color: m3u8sFlick.accentColor

                                Behavior on width { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
                            }
                        }
                    }

                    MutedText {
                        Layout.fillWidth: true
                        visible: appViewModel.m3u8sStatus.length > 0
                        text: appViewModel.m3u8sStatus
                        color: appViewModel.m3u8sPackaging ? theme.muted : m3u8sFlick.accentColor
                        wrapMode: Text.WordWrap
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                SectionHeader {
                    Layout.fillWidth: true
                    title: t("m3u8s.savedTitle")
                    subtitle: t("m3u8s.savedSubtitle")
                }

                ModernButton {
                    text: t("action.refresh")
                    onClicked: appViewModel.refreshTsslPackages()
                }

                ModernButton {
                    text: t("m3u8s.openStorage")
                    onClicked: appViewModel.openTsslStorageDirectory()
                }

                ModernButton {
                    text: t("m3u8s.importTssl")
                    onClicked: appViewModel.restoreManagedTssl()
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 10
                visible: appViewModel.tsslPackages.count > 0

                Repeater {
                    model: appViewModel.tsslPackages

                    delegate: Rectangle {
                        id: tsslRow
                        required property int index
                        required property string rootDigest
                        required property string identifierPreview
                        required property int identifierLength
                        required property var modifiedAt
                        required property real fileSize
                        required property int manifestCount
                        required property int segmentCount
                        required property int resourceCount
                        required property bool validPackage
                        required property string validationError

                        Layout.fillWidth: true
                        Layout.preferredHeight: 122
                        radius: 8
                        color: tsslMouse.containsMouse ? theme.elevatedHover : theme.elevated
                        border.color: !tsslRow.validPackage ? root.withAlpha(theme.danger, 0.66)
                            : tsslMouse.containsMouse
                            ? root.withAlpha(m3u8sFlick.accentColor, 0.72) : theme.border
                        opacity: tsslRow.validPackage ? 1.0 : 0.82

                        MouseArea {
                            id: tsslMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            acceptedButtons: Qt.NoButton
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 15
                            spacing: 14

                            ServiceTypeIcon {
                                Layout.preferredWidth: 48
                                Layout.preferredHeight: 48
                                Layout.alignment: Qt.AlignTop
                                serviceType: "M3u8s"
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                spacing: 5

                                RowLayout {
                                    Layout.fillWidth: true

                                    Label {
                                        Layout.fillWidth: true
                                        text: tsslRow.validPackage
                                            ? t("m3u8s.identifier") + "  " + tsslRow.identifierPreview
                                            : t("m3u8s.invalidSavedPackage")
                                        color: theme.text
                                        font.pixelSize: 14
                                        font.bold: true
                                        font.family: tsslRow.validPackage ? "monospace" : ""
                                        elide: Text.ElideMiddle
                                    }

                                    Rectangle {
                                        visible: tsslRow.validPackage
                                        Layout.preferredWidth: identifierLengthLabel.implicitWidth + 18
                                        Layout.preferredHeight: 23
                                        radius: 7
                                        color: root.withAlpha(m3u8sFlick.accentColor, darkTheme ? 0.18 : 0.10)
                                        border.color: root.withAlpha(m3u8sFlick.accentColor, 0.48)

                                        Label {
                                            id: identifierLengthLabel
                                            anchors.centerIn: parent
                                            text: t("m3u8s.identifierLength").arg(tsslRow.identifierLength)
                                            color: m3u8sFlick.accentColor
                                            font.pixelSize: 10
                                            font.bold: true
                                        }
                                    }
                                }

                                MutedText {
                                    Layout.fillWidth: true
                                    text: "SHA-256  " + tsslRow.rootDigest
                                    font.family: "monospace"
                                    font.pixelSize: 11
                                    elide: Text.ElideMiddle
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 14

                                    Label {
                                        Layout.fillWidth: !tsslRow.validPackage
                                        text: tsslRow.validPackage
                                            ? t("m3u8s.segments").arg(tsslRow.segmentCount)
                                            : tsslRow.validationError
                                        color: tsslRow.validPackage ? theme.muted : theme.danger
                                        font.pixelSize: 12
                                        font.bold: true
                                        elide: Text.ElideRight
                                    }

                                    MutedText {
                                        text: root.formatBytes(tsslRow.fileSize)
                                        font.pixelSize: 12
                                    }

                                    MutedText {
                                        text: Qt.formatDateTime(tsslRow.modifiedAt, "yyyy-MM-dd  HH:mm")
                                        font.pixelSize: 12
                                    }

                                    Item { Layout.fillWidth: true }
                                }
                            }

                            ColumnLayout {
                                Layout.preferredWidth: 108
                                Layout.fillHeight: true
                                spacing: 8

                                ModernButton {
                                    Layout.fillWidth: true
                                    text: t("m3u8s.exportTssl")
                                    enabled: tsslRow.validPackage
                                    onClicked: appViewModel.exportManagedTssl(tsslRow.index)
                                }

                                ModernButton {
                                    Layout.fillWidth: true
                                    text: t("action.delete")
                                    danger: true
                                    onClicked: {
                                        root.pendingTsslDeleteRow = tsslRow.index
                                        tsslDeleteDialog.open()
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 170
                visible: appViewModel.tsslPackages.count === 0
                radius: 8
                color: theme.surface
                border.color: theme.border

                ColumnLayout {
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 48, 460)
                    spacing: 9

                    ServiceTypeIcon {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.preferredWidth: 50
                        Layout.preferredHeight: 50
                        serviceType: "M3u8s"
                    }

                    Label {
                        Layout.fillWidth: true
                        text: t("m3u8s.noPackages")
                        color: theme.text
                        font.pixelSize: 17
                        font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                    }

                    MutedText {
                        Layout.fillWidth: true
                        text: t("m3u8s.noPackagesHint")
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                    }
                }
            }

            Item { Layout.preferredHeight: 8 }
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
                    label: t("settings.embyHomeLayout")

                    HomeLayoutSelector {
                        selectedLayout: appViewModel.embyHomeLayout
                        onLayoutChosen: function(value) {
                            appViewModel.embyHomeLayout = value
                        }
                    }
                }

                SettingRow {
                    label: t("settings.jellyfinHomeLayout")

                    HomeLayoutSelector {
                        selectedLayout: appViewModel.jellyfinHomeLayout
                        onLayoutChosen: function(value) {
                            appViewModel.jellyfinHomeLayout = value
                        }
                    }
                }

                SettingRow {
                    label: t("settings.playerLayout")

                    HomeLayoutSelector {
                        selectedLayout: appViewModel.playerLayout
                        trendyLabel: t("option.playerTrendy")
                        traditionalLabel: t("option.playerTraditional")
                        onLayoutChosen: function(value) {
                            appViewModel.playerLayout = value
                        }
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

    component HomeLayoutSelector: Rectangle {
        id: homeLayoutSelector
        property string selectedLayout: "trendy"
        property string trendyLabel: t("option.homeTrendy")
        property string traditionalLabel: t("option.homeTraditional")
        signal layoutChosen(string value)

        Layout.preferredWidth: 220
        Layout.preferredHeight: 40
        radius: 8
        color: theme.input
        border.color: theme.border

        RowLayout {
            anchors.fill: parent
            anchors.margins: 3
            spacing: 3

            Button {
                id: trendyHomeLayoutButton
                Layout.fillWidth: true
                Layout.fillHeight: true
                text: homeLayoutSelector.trendyLabel
                onClicked: homeLayoutSelector.layoutChosen("trendy")

                contentItem: Label {
                    text: trendyHomeLayoutButton.text
                    color: homeLayoutSelector.selectedLayout === "trendy"
                        ? "#ffffff" : theme.text
                    font.pixelSize: 13
                    font.bold: homeLayoutSelector.selectedLayout === "trendy"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                background: Rectangle {
                    radius: 6
                    color: homeLayoutSelector.selectedLayout === "trendy"
                        ? theme.primary
                        : trendyHomeLayoutButton.hovered ? theme.elevatedHover : "transparent"
                }
            }

            Button {
                id: traditionalHomeLayoutButton
                Layout.fillWidth: true
                Layout.fillHeight: true
                text: homeLayoutSelector.traditionalLabel
                onClicked: homeLayoutSelector.layoutChosen("traditional")

                contentItem: Label {
                    text: traditionalHomeLayoutButton.text
                    color: homeLayoutSelector.selectedLayout === "traditional"
                        ? "#ffffff" : theme.text
                    font.pixelSize: 13
                    font.bold: homeLayoutSelector.selectedLayout === "traditional"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                background: Rectangle {
                    radius: 6
                    color: homeLayoutSelector.selectedLayout === "traditional"
                        ? theme.primary
                        : traditionalHomeLayoutButton.hovered ? theme.elevatedHover : "transparent"
                }
            }
        }
    }
}
