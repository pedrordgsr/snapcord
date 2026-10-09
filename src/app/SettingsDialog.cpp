#include "SettingsDialog.h"

#include "Language.h"
#include "Motion.h"
#include "Notifier.h"
#include "RichPresence.h"
#include "Theme.h"
#include "UpdateDialog.h"
#include "VoiceController.h"
#include "core/UpdateChecker.h"
#include "platform/KeyState.h"
#include "voice/AudioEngine.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QFileDialog>
#include <QFrame>
#include <QIcon>
#include <QLocale>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QProcess>
#include <QRadioButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "core/Session.h"

#include <cmath>

namespace {

constexpr float MinDb = -100.0f;

QLabel* sectionLabel(const QString& text)
{
    auto* label = new QLabel(text.toUpper());
    label->setObjectName(QStringLiteral("settingsSection"));
    return label;
}

// A checkbox with a muted explanation underneath, like Discord's settings toggles.
QWidget* option(QCheckBox* checkBox, const QString& hint)
{
    auto* widget = new QWidget;
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 4);
    layout->setSpacing(2);
    layout->addWidget(checkBox);
    auto* label = new QLabel(hint);
    label->setObjectName(QStringLiteral("settingsHint"));
    label->setWordWrap(true);
    label->setContentsMargins(26, 0, 0, 0);
    layout->addWidget(label);
    return widget;
}

QSlider* volumeSlider(float value)
{
    auto* slider = new QSlider(Qt::Horizontal);
    slider->setRange(0, 200);
    slider->setValue(qRound(value * 100));
    slider->setToolTip(QStringLiteral("%1%").arg(slider->value()));
    QObject::connect(slider, &QSlider::valueChanged, slider,
                     [slider](int v) { slider->setToolTip(QStringLiteral("%1%").arg(v)); });
    return slider;
}

} // namespace

// --- LevelMeter -------------------------------------------------------------------------------------

LevelMeter::LevelMeter(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(24);
    setCursor(Qt::PointingHandCursor);
    setToolTip(tr("Drag to set how loud you need to be for your microphone to activate."));
}

void LevelMeter::setLevel(float db)
{
    if (std::abs(db - m_level) < 0.5f)
        return;
    m_level = db;
    update();
}

void LevelMeter::setThreshold(float db)
{
    m_threshold = qBound(MinDb, db, 0.0f);
    update();
}

void LevelMeter::setThresholdVisible(bool visible)
{
    m_thresholdVisible = visible;
    update();
}

float LevelMeter::dbAt(int x) const
{
    return MinDb + (qBound(0, x, width()) / float(width())) * -MinDb;
}

void LevelMeter::mousePressEvent(QMouseEvent* event)
{
    if (!m_thresholdVisible)
        return;
    setThreshold(dbAt(event->position().toPoint().x()));
    emit thresholdChanged(m_threshold);
}

void LevelMeter::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_thresholdVisible || !(event->buttons() & Qt::LeftButton))
        return;
    setThreshold(dbAt(event->position().toPoint().x()));
    emit thresholdChanged(m_threshold);
}

void LevelMeter::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF bar = QRectF(rect()).adjusted(0, 6, 0, -6);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Theme::instance().palette().button);
    painter.drawRoundedRect(bar, 4, 4);

    const qreal levelX = bar.width() * (qBound(MinDb, m_level, 0.0f) - MinDb) / -MinDb;
    const bool active = !m_thresholdVisible || m_level >= m_threshold;
    const auto& palette = Theme::instance().palette();
    painter.setBrush(active ? palette.success : palette.warning);
    painter.drawRoundedRect(QRectF(bar.left(), bar.top(), levelX, bar.height()), 4, 4);

    if (m_thresholdVisible) {
        const qreal thresholdX = bar.width() * (m_threshold - MinDb) / -MinDb;
        painter.setBrush(Qt::white);
        painter.drawRoundedRect(QRectF(thresholdX - 3, 0, 6, height()), 3, 3);
    }
}

// --- KeybindButton ----------------------------------------------------------------------------------

KeybindButton::KeybindButton(QWidget* parent)
    : QPushButton(parent)
{
    setObjectName(QStringLiteral("secondaryButton"));
    setCursor(Qt::PointingHandCursor);
    setMinimumWidth(200);
    connect(this, &QPushButton::clicked, this, [this] {
        m_recording = true;
        updateText();
        setFocus();
    });
    updateText();
}

void KeybindButton::setKey(int nativeKey)
{
    m_key = nativeKey;
    updateText();
}

void KeybindButton::keyPressEvent(QKeyEvent* event)
{
    if (!m_recording) {
        QPushButton::keyPressEvent(event);
        return;
    }
    if (event->key() != Qt::Key_Escape) {
        const int key = static_cast<int>(event->nativeVirtualKey());
        if (key != 0) {
            m_key = key;
            emit keyChanged(m_key);
        }
    }
    stopRecording();
}

void KeybindButton::mousePressEvent(QMouseEvent* event)
{
    if (m_recording) {
        const int key = KeyState::mouseButtonKey(event->button());
        if (key != 0) {
            m_key = key;
            emit keyChanged(m_key);
            stopRecording();
            return;
        }
    }
    QPushButton::mousePressEvent(event);
}

void KeybindButton::focusOutEvent(QFocusEvent* event)
{
    stopRecording();
    QPushButton::focusOutEvent(event);
}

void KeybindButton::stopRecording()
{
    m_recording = false;
    updateText();
}

void KeybindButton::updateText()
{
    if (m_recording)
        setText(tr("Press a key…"));
    else
        setText(m_key == 0 ? tr("Record Keybind") : KeyState::name(m_key));
}

// --- ColorSwatch ------------------------------------------------------------------------------------

ColorSwatch::ColorSwatch(QWidget* parent)
    : QPushButton(parent)
{
    setObjectName(QStringLiteral("colorSwatch"));
    setCursor(Qt::PointingHandCursor);
    setFixedSize(40, 40);
    setFocusPolicy(Qt::NoFocus);
    setCheckable(false);
}

void ColorSwatch::setSwatchColor(const QColor& color)
{
    if (m_color == color)
        return;
    m_color = color.isValid() ? color : QColor(Qt::black);
    update();
}

void ColorSwatch::setSelectedSwatch(bool selected)
{
    if (m_selected == selected)
        return;
    m_selected = selected;
    update();
}

void ColorSwatch::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF box = QRectF(rect()).adjusted(1, 1, -1, -1);
    const QColor ring = m_selected || underMouse() ? Theme::instance().accent()
                                                   : Theme::instance().palette().border;
    painter.setPen(QPen(ring, m_selected ? 3 : 2));
    painter.setBrush(m_color);
    painter.drawRoundedRect(box, 6, 6);
}

// --- LayoutStudio -----------------------------------------------------------------------------------

LayoutStudio::LayoutStudio(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    setToolTip(tr("Click a region to change its color"));
    connect(&Theme::instance(), &Theme::changed, this, QOverload<>::of(&QWidget::update));
}

QString LayoutStudio::hitTest(const QPoint& pos) const
{
    const QRect area = rect().adjusted(8, 8, -8, -8);
    if (!area.contains(pos))
        return {};
    const int railW = qMax(18, area.width() / 10);
    const int sideW = qMax(48, area.width() / 4);
    const QRect rail(area.left(), area.top(), railW, area.height());
    const QRect side(rail.right() + 1, area.top(), sideW, area.height());
    const QRect chat(side.right() + 1, area.top(), area.right() - side.right(), area.height());
    const QRect accent(chat.left() + 12, chat.bottom() - 18, 40, 8);
    if (accent.contains(pos))
        return QStringLiteral("accent");
    if (rail.contains(pos))
        return QStringLiteral("bg0");
    if (side.contains(pos))
        return QStringLiteral("bg1");
    if (chat.contains(pos))
        return QStringLiteral("bg2");
    return {};
}

void LayoutStudio::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const Theme::Palette& c = Theme::instance().palette();
    // Fill opaque first — parent scroll views can otherwise bleed a light Fusion Window color.
    painter.fillRect(rect(), c.bg2);
    const QRect area = rect().adjusted(8, 8, -8, -8);
    const int railW = qMax(18, area.width() / 10);
    const int sideW = qMax(48, area.width() / 4);
    const QRect rail(area.left(), area.top(), railW, area.height());
    const QRect side(rail.right() + 1, area.top(), sideW, area.height());
    const QRect chat(side.right() + 1, area.top(), area.right() - side.right(), area.height());

    painter.setPen(QPen(c.border, 1.5));
    painter.setBrush(c.bg2);
    painter.drawRoundedRect(area, 10, 10);

    auto drawRegion = [&](const QRect& r, const QColor& fill, const QString& id, const QString& label) {
        const bool hot = m_hover == id;
        painter.setPen(hot ? QPen(c.accent, 2) : Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRect(r);
        painter.setPen(c.textBright);
        QFont font = painter.font();
        font.setPixelSize(11);
        font.setWeight(QFont::DemiBold);
        painter.setFont(font);
        painter.drawText(r.adjusted(6, 0, -6, 0), Qt::AlignCenter, label);
    };

    drawRegion(rail, c.bg0, QStringLiteral("bg0"), tr("Rail"));
    drawRegion(side, c.bg1, QStringLiteral("bg1"), tr("Sidebar"));
    drawRegion(chat, c.bg2, QStringLiteral("bg2"), tr("Chat"));

    const QRect accent(chat.left() + 12, chat.bottom() - 18, 48, 10);
    painter.setPen(m_hover == u"accent" ? QPen(c.textBright, 1) : Qt::NoPen);
    painter.setBrush(c.accent);
    painter.drawRoundedRect(accent, 3, 3);
    painter.setPen(c.onAccent);
    painter.drawText(accent, Qt::AlignCenter, tr("Accent"));
}

void LayoutStudio::mousePressEvent(QMouseEvent* event)
{
    const QString id = hitTest(event->position().toPoint());
    if (!id.isEmpty())
        emit regionClicked(id);
}

void LayoutStudio::mouseMoveEvent(QMouseEvent* event)
{
    const QString id = hitTest(event->position().toPoint());
    if (id != m_hover) {
        m_hover = id;
        update();
    }
}

void LayoutStudio::leaveEvent(QEvent*)
{
    if (!m_hover.isEmpty()) {
        m_hover.clear();
        update();
    }
}

// --- SettingsDialog ---------------------------------------------------------------------------------

SettingsDialog::SettingsDialog(VoiceController* voice, QWidget* parent)
    : QDialog(parent)
    , m_voice(voice)
    , m_settings(VoiceSettings::load())
    , m_meterTimer(new QTimer(this))
{
    setObjectName(QStringLiteral("settingsDialog"));
    setAttribute(Qt::WA_StyledBackground, true);
    setWindowTitle(tr("User Settings"));
    resize(860, 640);

    auto* navigation = new QListWidget;
    navigation->setObjectName(QStringLiteral("settingsNavigation"));
    navigation->setFixedWidth(200);
    navigation->setFocusPolicy(Qt::NoFocus);
    navigation->addItem(tr("Voice & Audio"));
    navigation->addItem(tr("Sound Effects"));
    navigation->addItem(tr("Appearance"));
    navigation->addItem(tr("Notifications"));
    if constexpr (RichPresence::Enabled)
        navigation->addItem(tr("Activity Privacy"));
    navigation->addItem(tr("Language"));
    navigation->addItem(tr("About"));

    auto* logout = new QPushButton(tr("Log Out"));
    logout->setObjectName(QStringLiteral("dangerButton"));
    logout->setCursor(Qt::PointingHandCursor);
    connect(logout, &QPushButton::clicked, this, [this] {
        accept();
        emit logoutRequested();
    });

    m_settingsSide = new QWidget;
    m_settingsSide->setObjectName(QStringLiteral("settingsSide"));
    m_settingsSide->setAttribute(Qt::WA_StyledBackground, true);
    auto* sideLayout = new QVBoxLayout(m_settingsSide);
    sideLayout->setContentsMargins(12, 24, 12, 16);
    sideLayout->addWidget(navigation, 1);
    sideLayout->addWidget(logout);

    m_settingsPages = new QStackedWidget;
    m_settingsPages->addWidget(buildVoicePage());
    m_settingsPages->addWidget(buildSoundsPage());
    m_settingsPages->addWidget(buildAppearancePage());
    m_settingsPages->addWidget(buildNotificationsPage());
    if constexpr (RichPresence::Enabled)
        m_settingsPages->addWidget(buildActivityPage());
    m_settingsPages->addWidget(buildLanguagePage());
    m_settingsPages->addWidget(buildAboutPage());
    connect(navigation, &QListWidget::currentRowChanged, m_settingsPages, [this](int row) {
        Motion::crossFade(m_settingsPages);
        m_settingsPages->setCurrentIndex(row);
        // Contributors come from GitHub only when someone actually opens About (once per run).
        if (m_settingsPages->currentWidget() == m_aboutPage)
            UpdateChecker::instance().fetchContributors();
    });
    navigation->setCurrentRow(0);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_settingsSide);
    layout->addWidget(m_settingsPages, 1);

    m_meterTimer->setInterval(50);
    connect(m_meterTimer, &QTimer::timeout, this, [this] {
        const bool inCall = m_voice->state() == VoiceConnection::State::Connected;
        m_meter->setLevel(inCall ? m_voice->connection()->inputLevelDb() : m_testLevelDb.load());
    });

    connect(&Theme::instance(), &Theme::changed, this, &SettingsDialog::applyDialogChrome);
    applyDialogChrome();
}

SettingsDialog::~SettingsDialog()
{
    stopMicTest();
}

QWidget* SettingsDialog::buildVoicePage()
{
    AudioEngine& audio = m_voice->connection()->audio();

    m_inputDevice = new QComboBox;
    m_inputDevice->addItem(tr("Default"), QString());
    for (const QString& name : audio.inputDevices())
        m_inputDevice->addItem(name, name);
    m_inputDevice->setCurrentIndex(qMax(0, m_inputDevice->findData(m_settings.inputDevice)));

    m_outputDevice = new QComboBox;
    m_outputDevice->addItem(tr("Default"), QString());
    for (const QString& name : audio.outputDevices())
        m_outputDevice->addItem(name, name);
    m_outputDevice->setCurrentIndex(qMax(0, m_outputDevice->findData(m_settings.outputDevice)));

    // Device names can be very long; without this the page grows wider than the window.
    for (QComboBox* combo : {m_inputDevice, m_outputDevice}) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(12);
    }

    m_inputVolume = volumeSlider(m_settings.inputVolume);
    m_outputVolume = volumeSlider(m_settings.outputVolume);

    m_voiceActivity = new QRadioButton(tr("Voice Activity"));
    m_pushToTalk = new QRadioButton(tr("Push to Talk"));
    auto* modeGroup = new QButtonGroup(this);
    modeGroup->addButton(m_voiceActivity);
    modeGroup->addButton(m_pushToTalk);
    (m_settings.inputMode == VoiceSettings::InputMode::PushToTalk ? m_pushToTalk : m_voiceActivity)->setChecked(true);

    m_meter = new LevelMeter;
    m_meter->setThreshold(m_settings.activationThresholdDb);
    m_automaticSensitivity = new QCheckBox(tr("Automatically determine input sensitivity"));
    m_automaticSensitivity->setChecked(m_settings.automaticSensitivity);

    m_noiseSuppression = new QCheckBox(tr("Noise Suppression"));
    m_noiseSuppression->setChecked(m_settings.noiseSuppression);
    m_echoCancellation = new QCheckBox(tr("Echo Cancellation"));
    m_echoCancellation->setChecked(m_settings.echoCancellation);
    m_automaticGainControl = new QCheckBox(tr("Automatic Gain Control"));
    m_automaticGainControl->setChecked(m_settings.automaticGainControl);

    m_keybind = new KeybindButton;
    m_keybind->setKey(m_settings.pushToTalkKey);
    m_releaseDelay = new QSlider(Qt::Horizontal);
    m_releaseDelay->setRange(0, 2000);
    m_releaseDelay->setSingleStep(20);
    m_releaseDelay->setValue(m_settings.pushToTalkReleaseMs);
    m_releaseDelayLabel = new QLabel;
    m_releaseDelayLabel->setObjectName(QStringLiteral("settingsHint"));

    // Layout.
    auto* devices = new QHBoxLayout;
    devices->setSpacing(16);
    auto* inputColumn = new QVBoxLayout;
    inputColumn->addWidget(sectionLabel(tr("Input Device")));
    inputColumn->addWidget(m_inputDevice);
    inputColumn->addSpacing(8);
    inputColumn->addWidget(sectionLabel(tr("Input Volume")));
    inputColumn->addWidget(m_inputVolume);
    auto* outputColumn = new QVBoxLayout;
    outputColumn->addWidget(sectionLabel(tr("Output Device")));
    outputColumn->addWidget(m_outputDevice);
    outputColumn->addSpacing(8);
    outputColumn->addWidget(sectionLabel(tr("Output Volume")));
    outputColumn->addWidget(m_outputVolume);
    devices->addLayout(inputColumn, 1);
    devices->addLayout(outputColumn, 1);

    m_sensitivityGroup = new QWidget;
    auto* sensitivityLayout = new QVBoxLayout(m_sensitivityGroup);
    sensitivityLayout->setContentsMargins(0, 0, 0, 0);
    sensitivityLayout->addWidget(sectionLabel(tr("Input Sensitivity")));
    sensitivityLayout->addWidget(m_automaticSensitivity);
    auto* sensitivityHint = new QLabel(tr("Talk to test your microphone. The bar turns green when you are loud "
                                          "enough to be heard; drag the white marker to adjust."));
    sensitivityHint->setObjectName(QStringLiteral("settingsHint"));
    sensitivityHint->setWordWrap(true);
    sensitivityLayout->addWidget(sensitivityHint);
    sensitivityLayout->addWidget(m_meter);

    m_pushToTalkGroup = new QWidget;
    auto* pttLayout = new QVBoxLayout(m_pushToTalkGroup);
    pttLayout->setContentsMargins(0, 0, 0, 0);
    auto* pttRow = new QHBoxLayout;
    auto* shortcutColumn = new QVBoxLayout;
    shortcutColumn->addWidget(sectionLabel(tr("Shortcut")));
    shortcutColumn->addWidget(m_keybind);
    auto* delayColumn = new QVBoxLayout;
    delayColumn->addWidget(sectionLabel(tr("Push to Talk Release Delay")));
    delayColumn->addWidget(m_releaseDelay);
    delayColumn->addWidget(m_releaseDelayLabel);
    pttRow->addLayout(shortcutColumn);
    pttRow->addSpacing(16);
    pttRow->addLayout(delayColumn, 1);
    pttLayout->addLayout(pttRow);
    if (!KeyState::isSupported()) {
        auto* unsupported = new QLabel(tr("Push to Talk is not available on this system yet."));
        unsupported->setObjectName(QStringLiteral("settingsHint"));
        pttLayout->addWidget(unsupported);
    }

    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("settingsContent"));
    content->setAttribute(Qt::WA_StyledBackground);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(40, 32, 40, 32);
    layout->setSpacing(12);
    auto* title = new QLabel(tr("Voice & Audio"));
    title->setObjectName(QStringLiteral("settingsTitle"));
    layout->addWidget(title);
    layout->addSpacing(8);
    layout->addLayout(devices);
    layout->addSpacing(16);
    layout->addWidget(sectionLabel(tr("Input Mode")));
    layout->addWidget(m_voiceActivity);
    layout->addWidget(m_pushToTalk);
    layout->addSpacing(16);
    layout->addWidget(m_sensitivityGroup);
    layout->addWidget(m_pushToTalkGroup);
    layout->addSpacing(16);
    layout->addWidget(sectionLabel(tr("Voice Processing")));
    layout->addWidget(option(m_noiseSuppression, tr("Removes background noise like keyboards, fans and traffic.")));
    layout->addWidget(option(m_echoCancellation,
                             tr("Stops others from hearing themselves when you use speakers instead of headphones.")));
    layout->addWidget(option(m_automaticGainControl, tr("Keeps your voice at a steady volume.")));
    layout->addStretch();

    connect(m_inputDevice, &QComboBox::currentIndexChanged, this, [this] {
        apply();
        if (m_testAudio)
            startMicTest();
    });
    connect(m_outputDevice, &QComboBox::currentIndexChanged, this, &SettingsDialog::apply);
    connect(m_inputVolume, &QSlider::valueChanged, this, &SettingsDialog::apply);
    connect(m_outputVolume, &QSlider::valueChanged, this, &SettingsDialog::apply);
    connect(m_voiceActivity, &QRadioButton::toggled, this, [this] {
        updateModeWidgets();
        apply();
    });
    connect(m_meter, &LevelMeter::thresholdChanged, this, &SettingsDialog::apply);
    connect(m_automaticSensitivity, &QCheckBox::toggled, this, [this] {
        updateModeWidgets();
        apply();
    });
    for (QCheckBox* checkBox : {m_noiseSuppression, m_echoCancellation, m_automaticGainControl})
        connect(checkBox, &QCheckBox::toggled, this, &SettingsDialog::apply);
    connect(m_keybind, &KeybindButton::keyChanged, this, &SettingsDialog::apply);
    connect(m_releaseDelay, &QSlider::valueChanged, this, [this] {
        m_releaseDelayLabel->setText(tr("%1 ms").arg(m_releaseDelay->value()));
        apply();
    });
    m_releaseDelayLabel->setText(tr("%1 ms").arg(m_releaseDelay->value()));
    updateModeWidgets();

    auto* scroll = new QScrollArea;
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    return scroll;
}

QWidget* SettingsDialog::buildSoundsPage()
{
    using Sound = SoundEffects::Sound;
    using Style = SoundEffects::Style;
    SoundEffects* sounds = m_voice->sounds();

    m_soundEffects = new QCheckBox(tr("Play sound effects"));
    m_soundEffects->setChecked(m_settings.soundEffects);
    m_participantMuteSounds = new QCheckBox(tr("Play a sound when others mute or unmute"));
    m_participantMuteSounds->setChecked(m_settings.participantMuteSounds);
    for (QCheckBox* checkBox : {m_soundEffects, m_participantMuteSounds})
        connect(checkBox, &QCheckBox::toggled, this, &SettingsDialog::apply);

    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("settingsContent"));
    content->setAttribute(Qt::WA_StyledBackground);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(40, 32, 40, 32);
    layout->setSpacing(12);
    auto* title = new QLabel(tr("Sound Effects"));
    title->setObjectName(QStringLiteral("settingsTitle"));
    layout->addWidget(title);
    layout->addSpacing(8);
    layout->addWidget(option(m_soundEffects, tr("Joining, leaving, muting and incoming calls.")));
    layout->addWidget(option(m_participantMuteSounds,
                             tr("While you are in a call, hear when someone else in it mutes or unmutes.")));
    layout->addSpacing(16);
    layout->addWidget(sectionLabel(tr("Sounds")));

    const std::pair<Sound, QString> rows[] = {
        {Sound::Join, tr("You join a call")},
        {Sound::Leave, tr("You leave a call")},
        {Sound::UserJoin, tr("Someone joins")},
        {Sound::UserLeave, tr("Someone leaves")},
        {Sound::Mute, tr("Mute")},
        {Sound::Unmute, tr("Unmute")},
        {Sound::Deafen, tr("Deafen")},
        {Sound::Undeafen, tr("Undeafen")},
        {Sound::UserMute, tr("Someone mutes")},
        {Sound::UserUnmute, tr("Someone unmutes")},
        {Sound::Ringtone, tr("Incoming call ringtone")},
        {Sound::Message, tr("New message")},
    };
    const std::pair<Style, QString> styles[] = {
        {Style::Classic, tr("Classic")}, {Style::Soft, tr("Soft")}, {Style::Digital, tr("Digital")},
        {Style::Pop, tr("Pop")},         {Style::Off, tr("Off")},
    };

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(8);
    grid->setColumnStretch(0, 1);
    int row = 0;
    for (const auto& [sound, name] : rows) {
        auto* label = new QLabel(name);
        auto* combo = new QComboBox;
        for (const auto& [style, styleName] : styles)
            combo->addItem(styleName, static_cast<int>(style));
        combo->setCurrentIndex(qMax(0, combo->findData(static_cast<int>(sounds->style(sound)))));
        combo->setMinimumWidth(140);
        ignoreWheel(combo);
        auto* play = new QPushButton(tr("Preview"));
        play->setObjectName(QStringLiteral("secondaryButton"));
        play->setCursor(Qt::PointingHandCursor);

        connect(combo, &QComboBox::currentIndexChanged, this, [sounds, combo, play, sound] {
            const auto style = static_cast<Style>(combo->currentData().toInt());
            sounds->setStyle(sound, style);
            sounds->saveStyles();
            play->setEnabled(style != Style::Off);
            sounds->preview(sound, style);
        });
        connect(play, &QPushButton::clicked, this, [sounds, combo, sound] {
            sounds->preview(sound, static_cast<Style>(combo->currentData().toInt()));
        });
        play->setEnabled(sounds->style(sound) != Style::Off);

        grid->addWidget(label, row, 0);
        grid->addWidget(combo, row, 1);
        grid->addWidget(play, row, 2);
        ++row;
    }
    layout->addLayout(grid);
    layout->addStretch();

    auto* scroll = new QScrollArea;
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    return scroll;
}

QWidget* SettingsDialog::buildNotificationsPage()
{
    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("settingsContent"));
    content->setAttribute(Qt::WA_StyledBackground);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(40, 32, 40, 32);
    layout->setSpacing(12);
    auto* title = new QLabel(tr("Notifications"));
    title->setObjectName(QStringLiteral("settingsTitle"));
    layout->addWidget(title);
    layout->addSpacing(8);

    auto* desktop = new QCheckBox(tr("Enable desktop notifications"));
    desktop->setChecked(Notifier::desktopNotificationsEnabled());
    connect(desktop, &QCheckBox::toggled, this, &Notifier::setDesktopNotificationsEnabled);
    layout->addWidget(option(desktop, tr("Direct messages and mentions show a notification while Snapcord is in the background.")));

    auto* sound = new QCheckBox(tr("Play a sound for new messages"));
    sound->setChecked(Notifier::soundEnabled());
    connect(sound, &QCheckBox::toggled, this, &Notifier::setSoundEnabled);
    layout->addWidget(option(sound, tr("Only for direct messages and mentions, like the notifications.")));
    layout->addStretch();
    return content;
}

QWidget* SettingsDialog::buildActivityPage()
{
    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("settingsContent"));
    content->setAttribute(Qt::WA_StyledBackground);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(40, 32, 40, 32);
    layout->setSpacing(12);
    auto* title = new QLabel(tr("Activity Privacy"));
    title->setObjectName(QStringLiteral("settingsTitle"));
    layout->addWidget(title);
    layout->addSpacing(8);

    auto* games = new QCheckBox(tr("Share the game you are playing"));
    games->setChecked(RichPresence::shareGames());
    connect(games, &QCheckBox::toggled, this, &RichPresence::setShareGames);
    layout->addWidget(option(games, tr("Games are recognized from the programs running on this computer, "
                                       "using Discord's list of detectable games.")));

    auto* spotify = new QCheckBox(tr("Display Spotify as your status"));
    spotify->setChecked(RichPresence::shareSpotify());
    connect(spotify, &QCheckBox::toggled, this, &RichPresence::setShareSpotify);
    layout->addWidget(option(spotify, tr("Shows the song you are listening to. Requires your Spotify account to be "
                                         "connected to Discord (Settings > Connections in the official app), with "
                                         "\"Display Spotify as your status\" turned on there.")));
    layout->addStretch();
    return content;
}

QWidget* SettingsDialog::buildAppearancePage()
{
    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("settingsContent"));
    content->setAttribute(Qt::WA_StyledBackground);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(40, 32, 40, 32);
    layout->setSpacing(12);

    auto* title = new QLabel(tr("Appearance"));
    title->setObjectName(QStringLiteral("settingsTitle"));
    layout->addWidget(title);
    layout->addSpacing(4);

    auto* hint = new QLabel(tr("Themes and colors stay on this device. They do not change your Discord profile."));
    hint->setObjectName(QStringLiteral("settingsHint"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addSpacing(8);

    layout->addWidget(sectionLabel(tr("Theme")));
    const Theme::Settings current = Theme::instance().settings();
    m_presetCombo = new QComboBox;
    for (const Theme::Preset& preset : Theme::presets())
        m_presetCombo->addItem(presetDisplayName(preset.id, preset.name), preset.id);
    {
        const int idx = m_presetCombo->findData(current.presetId);
        m_presetCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    }
    layout->addWidget(m_presetCombo);
    ignoreWheel(m_presetCombo);
    connect(m_presetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &SettingsDialog::applyAppearance);

    layout->addSpacing(12);
    layout->addWidget(sectionLabel(tr("Click to recolor")));
    auto* studioHint = new QLabel(tr("Click rail, sidebar, chat, or accent on the map. Changes apply instantly."));
    studioHint->setObjectName(QStringLiteral("settingsHint"));
    studioHint->setWordWrap(true);
    layout->addWidget(studioHint);
    m_layoutStudio = new LayoutStudio;
    layout->addWidget(m_layoutStudio);
    connect(m_layoutStudio, &LayoutStudio::regionClicked, this, [this](const QString& id) {
        pickColor(Theme::instance().tokenColor(id), Theme::tokenLabel(id), [this, id](const QColor& c) {
            Theme::instance().setTokenOverride(id, c);
            refreshColorSwatches();
        });
    });

    m_customizedLabel = new QLabel;
    m_customizedLabel->setObjectName(QStringLiteral("settingsHint"));
    layout->addWidget(m_customizedLabel);

    layout->addSpacing(12);
    layout->addWidget(sectionLabel(tr("Tone & scale")));
    auto addToneSlider = [&](const QString& title, QSlider*& slider, QLabel*& valueLabel, int min, int max,
                             int value, const std::function<QString(int)>& format) {
        layout->addWidget(new QLabel(title));
        slider = new QSlider(Qt::Horizontal);
        slider->setRange(min, max);
        slider->setValue(value);
        slider->setPageStep(1);
        slider->setSingleStep(1);
        valueLabel = new QLabel(format(value));
        valueLabel->setObjectName(QStringLiteral("settingsHint"));
        valueLabel->setMinimumWidth(48);
        valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto* row = new QHBoxLayout;
        row->addWidget(slider, 1);
        row->addWidget(valueLabel);
        layout->addLayout(row);
        bindLiveSlider(slider, valueLabel, format);
        ignoreWheel(slider);
    };
    addToneSlider(tr("Brightness"), m_brightness, m_brightnessLabel, -40, 40, current.brightness,
                  [](int v) { return v > 0 ? QStringLiteral("+%1").arg(v) : QString::number(v); });
    addToneSlider(tr("Saturation"), m_saturation, m_saturationLabel, -50, 50, current.saturation,
                  [](int v) { return v > 0 ? QStringLiteral("+%1").arg(v) : QString::number(v); });
    addToneSlider(tr("UI scale"), m_uiScale, m_uiScaleLabel, 85, 130, current.uiScale,
                  [](int v) { return QStringLiteral("%1%").arg(v); });
    addToneSlider(tr("Corner radius"), m_radius, m_radiusLabel, 0, 12, current.radius,
                  [](int v) { return QStringLiteral("%1 px").arg(v); });

    layout->addSpacing(12);
    layout->addWidget(sectionLabel(tr("Accent color")));
    auto* accentHint = new QLabel(tr("Pick a color or open the custom picker."));
    accentHint->setObjectName(QStringLiteral("settingsHint"));
    accentHint->setWordWrap(true);
    layout->addWidget(accentHint);

    auto* accentRow = new QHBoxLayout;
    accentRow->setSpacing(8);
    m_accentChips.clear();
    for (const QColor& color : Theme::accentSwatches()) {
        auto* chip = new ColorSwatch;
        chip->setFixedSize(32, 32);
        chip->setSwatchColor(color);
        chip->setToolTip(color.name(QColor::HexRgb).toUpper());
        accentRow->addWidget(chip);
        m_accentChips.push_back(chip);
        connect(chip, &QPushButton::clicked, this, [this, color] { setCustomAccent(color); });
    }
    m_accentSwatch = new ColorSwatch;
    m_accentSwatch->setToolTip(tr("Custom…"));
    accentRow->addWidget(m_accentSwatch);
    auto* resetAccent = new QPushButton(tr("Use theme default"));
    resetAccent->setObjectName(QStringLiteral("secondaryButton"));
    resetAccent->setCursor(Qt::PointingHandCursor);
    accentRow->addWidget(resetAccent);
    accentRow->addStretch();
    layout->addLayout(accentRow);
    connect(m_accentSwatch, &QPushButton::clicked, this, [this] {
        const Theme::Settings settings = Theme::instance().settings();
        const QColor initial = settings.customAccent.isValid() ? settings.customAccent
                                                               : Theme::instance().accent();
        pickColor(initial, tr("Accent color"), [this](const QColor& chosen) { setCustomAccent(chosen); });
    });
    connect(resetAccent, &QPushButton::clicked, this, [this] {
        setCustomAccent(QColor()); // invalid = follow the selected theme
    });

    layout->addSpacing(12);
    layout->addWidget(sectionLabel(tr("Backgrounds")));
    auto* bgHint = new QLabel(tr("Server rail, channel list, and chat area. Leave unset to follow the theme."));
    bgHint->setObjectName(QStringLiteral("settingsHint"));
    bgHint->setWordWrap(true);
    layout->addWidget(bgHint);
    auto* bgRow = new QHBoxLayout;
    bgRow->setSpacing(8);
    m_bg0Swatch = new ColorSwatch;
    m_bg0Swatch->setToolTip(tr("Server rail"));
    m_bg1Swatch = new ColorSwatch;
    m_bg1Swatch->setToolTip(tr("Channel list"));
    m_bg2Swatch = new ColorSwatch;
    m_bg2Swatch->setToolTip(tr("Chat area"));
    auto* resetBg = new QPushButton(tr("Reset backgrounds"));
    resetBg->setObjectName(QStringLiteral("secondaryButton"));
    resetBg->setCursor(Qt::PointingHandCursor);
    bgRow->addWidget(m_bg0Swatch);
    bgRow->addWidget(m_bg1Swatch);
    bgRow->addWidget(m_bg2Swatch);
    bgRow->addWidget(resetBg);
    bgRow->addStretch();
    layout->addLayout(bgRow);
    connect(m_bg0Swatch, &QPushButton::clicked, this, [this] {
        pickColor(Theme::instance().palette().bg0, tr("Server rail"), [this](const QColor& c) {
            Theme::Settings s = Theme::instance().settings();
            s.customBg0 = c;
            Theme::instance().setSettings(s);
            refreshColorSwatches();
        });
    });
    connect(m_bg1Swatch, &QPushButton::clicked, this, [this] {
        pickColor(Theme::instance().palette().bg1, tr("Channel list"), [this](const QColor& c) {
            Theme::Settings s = Theme::instance().settings();
            s.customBg1 = c;
            Theme::instance().setSettings(s);
            refreshColorSwatches();
        });
    });
    connect(m_bg2Swatch, &QPushButton::clicked, this, [this] {
        pickColor(Theme::instance().palette().bg2, tr("Chat area"), [this](const QColor& c) {
            Theme::Settings s = Theme::instance().settings();
            s.customBg2 = c;
            Theme::instance().setSettings(s);
            refreshColorSwatches();
        });
    });
    connect(resetBg, &QPushButton::clicked, this, [this] {
        Theme::Settings s = Theme::instance().settings();
        s.customBg0 = QColor();
        s.customBg1 = QColor();
        s.customBg2 = QColor();
        Theme::instance().setSettings(s);
        refreshColorSwatches();
    });

    layout->addSpacing(12);
    layout->addWidget(sectionLabel(tr("My profile (this app only)")));
    auto* profileHint = new QLabel(tr("Colors for your user panel at the bottom left."));
    profileHint->setObjectName(QStringLiteral("settingsHint"));
    profileHint->setWordWrap(true);
    layout->addWidget(profileHint);

    auto* profileRow = new QHBoxLayout;
    m_profilePrimarySwatch = new ColorSwatch;
    m_profilePrimarySwatch->setToolTip(tr("Panel background"));
    m_profileAccentSwatch = new ColorSwatch;
    m_profileAccentSwatch->setToolTip(tr("Panel accent stripe"));
    auto* resetProfile = new QPushButton(tr("Reset profile colors"));
    resetProfile->setObjectName(QStringLiteral("secondaryButton"));
    resetProfile->setCursor(Qt::PointingHandCursor);
    profileRow->addWidget(m_profilePrimarySwatch);
    profileRow->addWidget(m_profileAccentSwatch);
    profileRow->addWidget(resetProfile);
    profileRow->addStretch();
    layout->addLayout(profileRow);

    connect(m_profilePrimarySwatch, &QPushButton::clicked, this, [this] {
        pickColor(Theme::instance().profilePrimary(), tr("Profile background"), [this](const QColor& chosen) {
            Theme::Settings settings = Theme::instance().settings();
            settings.profilePrimary = chosen;
            Theme::instance().setSettings(settings);
            refreshColorSwatches();
        });
    });
    connect(m_profileAccentSwatch, &QPushButton::clicked, this, [this] {
        pickColor(Theme::instance().profileAccent(), tr("Profile accent"), [this](const QColor& chosen) {
            Theme::Settings settings = Theme::instance().settings();
            settings.profileAccent = chosen;
            Theme::instance().setSettings(settings);
            refreshColorSwatches();
        });
    });
    connect(resetProfile, &QPushButton::clicked, this, [this] {
        Theme::Settings settings = Theme::instance().settings();
        settings.profilePrimary = QColor();
        settings.profileAccent = QColor();
        Theme::instance().setSettings(settings);
        refreshColorSwatches();
    });

    layout->addSpacing(12);
    layout->addWidget(sectionLabel(tr("Chat density")));
    m_chatDensity = new QComboBox;
    m_chatDensity->addItem(tr("Compact"), static_cast<int>(Theme::ChatDensity::Compact));
    m_chatDensity->addItem(tr("Normal"), static_cast<int>(Theme::ChatDensity::Normal));
    m_chatDensity->addItem(tr("Comfortable"), static_cast<int>(Theme::ChatDensity::Comfortable));
    m_chatDensity->setCurrentIndex(m_chatDensity->findData(static_cast<int>(current.chatDensity)));
    layout->addWidget(m_chatDensity);
    ignoreWheel(m_chatDensity);
    connect(m_chatDensity, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &SettingsDialog::applyAppearance);

    layout->addSpacing(12);
    layout->addWidget(sectionLabel(tr("Font")));
    m_fontFamily = new QComboBox;
    for (const QString& family : Theme::fontFamilyChoices()) {
        if (family.isEmpty())
            m_fontFamily->addItem(tr("Default (Noto / Inter)"), family);
        else
            m_fontFamily->addItem(family, family);
    }
    {
        const int idx = m_fontFamily->findData(current.fontFamily);
        m_fontFamily->setCurrentIndex(idx >= 0 ? idx : 0);
    }
    layout->addWidget(m_fontFamily);
    ignoreWheel(m_fontFamily);
    connect(m_fontFamily, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &SettingsDialog::applyAppearance);

    layout->addWidget(sectionLabel(tr("Chat font size")));
    m_fontSize = new QSlider(Qt::Horizontal);
    m_fontSize->setRange(12, 18);
    m_fontSize->setValue(current.fontSize);
    m_fontSize->setPageStep(1);
    m_fontSize->setSingleStep(1);
    m_fontSizeLabel = new QLabel(QStringLiteral("%1 px").arg(m_fontSize->value()));
    m_fontSizeLabel->setObjectName(QStringLiteral("settingsHint"));
    auto* fontRow = new QHBoxLayout;
    fontRow->addWidget(m_fontSize, 1);
    fontRow->addWidget(m_fontSizeLabel);
    layout->addLayout(fontRow);
    bindLiveSlider(m_fontSize, m_fontSizeLabel, [](int v) { return QStringLiteral("%1 px").arg(v); });
    ignoreWheel(m_fontSize);

    layout->addSpacing(12);
    layout->addWidget(sectionLabel(tr("Discord Nitro colors")));
    auto* nitroHint = new QLabel(
        tr("Uses your Discord accent_color / banner when the account has Nitro profile colors."));
    nitroHint->setObjectName(QStringLiteral("settingsHint"));
    nitroHint->setWordWrap(true);
    layout->addWidget(nitroHint);
    m_syncDiscordAccent = new QCheckBox(tr("Keep accent in sync with Discord"));
    m_syncDiscordAccent->setChecked(current.syncDiscordAccent);
    layout->addWidget(m_syncDiscordAccent);
    connect(m_syncDiscordAccent, &QCheckBox::toggled, this, &SettingsDialog::applyAppearance);
    auto* applyDiscord = new QPushButton(tr("Apply Discord colors now"));
    applyDiscord->setObjectName(QStringLiteral("secondaryButton"));
    applyDiscord->setCursor(Qt::PointingHandCursor);
    layout->addWidget(applyDiscord, 0, Qt::AlignLeft);
    connect(applyDiscord, &QPushButton::clicked, this, [this] {
        if (!m_voice || !m_voice->session())
            return;
        const User& self = m_voice->session()->self();
        const QColor accent = self.hasAccentColor ? QColor::fromRgb(self.accentColorRgb) : QColor();
        const QColor banner(self.bannerColorHex);
        Theme::instance().setDiscordProfileColors(accent, banner);
        Theme::Settings settings = Theme::instance().settings();
        if (accent.isValid())
            settings.customAccent = accent;
        if (banner.isValid())
            settings.profilePrimary = banner;
        Theme::instance().setSettings(settings);
        refreshAppearanceControls();
    });

    layout->addSpacing(12);
    layout->addWidget(sectionLabel(tr("Gradient")));
    m_gradientEnabled = new QCheckBox(tr("Enable chat gradient"));
    m_gradientEnabled->setChecked(current.gradientEnabled);
    layout->addWidget(m_gradientEnabled);
    connect(m_gradientEnabled, &QCheckBox::toggled, this, &SettingsDialog::applyAppearance);
    auto* gradRow = new QHBoxLayout;
    m_gradientTopSwatch = new ColorSwatch;
    m_gradientTopSwatch->setToolTip(tr("Gradient top"));
    m_gradientBottomSwatch = new ColorSwatch;
    m_gradientBottomSwatch->setToolTip(tr("Gradient bottom"));
    gradRow->addWidget(m_gradientTopSwatch);
    gradRow->addWidget(m_gradientBottomSwatch);
    gradRow->addStretch();
    layout->addLayout(gradRow);
    connect(m_gradientTopSwatch, &QPushButton::clicked, this, [this] {
        pickColor(Theme::instance().settings().gradientTop, tr("Gradient top"), [this](const QColor& c) {
            Theme::Settings s = Theme::instance().settings();
            s.gradientTop = c;
            Theme::instance().setSettings(s);
            refreshColorSwatches();
        });
    });
    connect(m_gradientBottomSwatch, &QPushButton::clicked, this, [this] {
        pickColor(Theme::instance().settings().gradientBottom, tr("Gradient bottom"), [this](const QColor& c) {
            Theme::Settings s = Theme::instance().settings();
            s.gradientBottom = c;
            Theme::instance().setSettings(s);
            refreshColorSwatches();
        });
    });

    layout->addSpacing(12);
    layout->addWidget(sectionLabel(tr("Advanced tokens")));
    auto* tokenHint = new QLabel(tr("Override any palette token. Cleared entries follow the theme again."));
    tokenHint->setObjectName(QStringLiteral("settingsHint"));
    tokenHint->setWordWrap(true);
    layout->addWidget(tokenHint);
    auto* tokenGrid = new QGridLayout;
    tokenGrid->setHorizontalSpacing(8);
    tokenGrid->setVerticalSpacing(6);
    m_tokenSwatches.clear();
    int tokenIndex = 0;
    for (const QString& id : Theme::tokenIds()) {
        auto* name = new QLabel(Theme::tokenLabel(id));
        name->setObjectName(QStringLiteral("settingsHint"));
        auto* chip = new ColorSwatch;
        chip->setFixedSize(28, 28);
        chip->setToolTip(id);
        tokenGrid->addWidget(name, tokenIndex, 0);
        tokenGrid->addWidget(chip, tokenIndex, 1);
        m_tokenSwatches.insert(id, chip);
        connect(chip, &QPushButton::clicked, this, [this, id] {
            pickColor(Theme::instance().tokenColor(id), Theme::tokenLabel(id), [this, id](const QColor& c) {
                Theme::instance().setTokenOverride(id, c);
                refreshColorSwatches();
            });
        });
        ++tokenIndex;
    }
    layout->addLayout(tokenGrid);
    auto* resetTokens = new QPushButton(tr("Reset all token overrides"));
    resetTokens->setObjectName(QStringLiteral("secondaryButton"));
    resetTokens->setCursor(Qt::PointingHandCursor);
    layout->addWidget(resetTokens, 0, Qt::AlignLeft);
    connect(resetTokens, &QPushButton::clicked, this, [this] {
        Theme::Settings settings = Theme::instance().settings();
        settings.tokenOverrides.clear();
        Theme::instance().setSettings(settings);
        refreshColorSwatches();
    });

    layout->addSpacing(12);
    layout->addWidget(sectionLabel(tr("Motion")));
    auto* reduceMotion = new QCheckBox(tr("Reduce motion"));
    reduceMotion->setChecked(Motion::reduceMotion());
    connect(reduceMotion, &QCheckBox::toggled, this, &Motion::setReduceMotion);
    layout->addWidget(option(reduceMotion, tr("Turns off hover fades, popup fades and other transitions.")));

    layout->addSpacing(12);
    layout->addWidget(sectionLabel(tr("Import / export")));
    auto* ioRow = new QHBoxLayout;
    auto* exportBtn = new QPushButton(tr("Export theme…"));
    exportBtn->setObjectName(QStringLiteral("secondaryButton"));
    exportBtn->setCursor(Qt::PointingHandCursor);
    auto* importBtn = new QPushButton(tr("Import theme…"));
    importBtn->setObjectName(QStringLiteral("secondaryButton"));
    importBtn->setCursor(Qt::PointingHandCursor);
    ioRow->addWidget(exportBtn);
    ioRow->addWidget(importBtn);
    ioRow->addStretch();
    layout->addLayout(ioRow);
    connect(exportBtn, &QPushButton::clicked, this, &SettingsDialog::exportTheme);
    connect(importBtn, &QPushButton::clicked, this, &SettingsDialog::importTheme);

    layout->addStretch();
    refreshColorSwatches();
    connect(&Theme::instance(), &Theme::changed, this, &SettingsDialog::refreshColorSwatches);

    m_appearanceScroll = new QScrollArea;
    m_appearanceScroll->setWidget(content);
    m_appearanceScroll->setWidgetResizable(true);
    m_appearanceScroll->setFrameShape(QFrame::NoFrame);
    m_appearanceScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_appearanceScroll->setAttribute(Qt::WA_StyledBackground, true);
    content->setMinimumWidth(0);
    return m_appearanceScroll;
}

void SettingsDialog::refreshColorSwatches()
{
    const Theme::Palette& palette = Theme::instance().palette();
    const Theme::Settings& appearance = Theme::instance().settings();
    if (m_accentSwatch)
        m_accentSwatch->setSwatchColor(Theme::instance().accent());
    if (m_bg0Swatch)
        m_bg0Swatch->setSwatchColor(palette.bg0);
    if (m_bg1Swatch)
        m_bg1Swatch->setSwatchColor(palette.bg1);
    if (m_bg2Swatch)
        m_bg2Swatch->setSwatchColor(palette.bg2);
    if (m_profilePrimarySwatch)
        m_profilePrimarySwatch->setSwatchColor(Theme::instance().profilePrimary());
    if (m_profileAccentSwatch)
        m_profileAccentSwatch->setSwatchColor(Theme::instance().profileAccent());
    if (m_gradientTopSwatch)
        m_gradientTopSwatch->setSwatchColor(appearance.gradientTop);
    if (m_gradientBottomSwatch)
        m_gradientBottomSwatch->setSwatchColor(appearance.gradientBottom);

    const QColor custom = appearance.customAccent;
    bool matchedChip = false;
    for (ColorSwatch* chip : m_accentChips) {
        if (!chip)
            continue;
        const bool match = custom.isValid() && chip->swatchColor().rgb() == custom.rgb();
        chip->setSelectedSwatch(match);
        matchedChip = matchedChip || match;
    }
    if (m_accentSwatch)
        m_accentSwatch->setSelectedSwatch(custom.isValid() && !matchedChip);

    for (auto it = m_tokenSwatches.begin(); it != m_tokenSwatches.end(); ++it) {
        if (!it.value())
            continue;
        it.value()->setSwatchColor(Theme::instance().tokenColor(it.key()));
        it.value()->setSelectedSwatch(appearance.tokenOverrides.contains(it.key()));
    }

    if (m_customizedLabel) {
        m_customizedLabel->setText(Theme::instance().hasCustomization()
                                       ? tr("This theme has local customizations.")
                                       : tr("Using the preset as-is."));
    }
}

void SettingsDialog::refreshAppearanceControls()
{
    m_syncingAppearance = true;
    const Theme::Settings& appearance = Theme::instance().settings();
    const QSignalBlocker blockFont(m_fontSize);
    const QSignalBlocker blockFamily(m_fontFamily);
    const QSignalBlocker blockRadius(m_radius);
    const QSignalBlocker blockScale(m_uiScale);
    const QSignalBlocker blockBright(m_brightness);
    const QSignalBlocker blockSat(m_saturation);
    const QSignalBlocker blockDensity(m_chatDensity);
    const QSignalBlocker blockSync(m_syncDiscordAccent);
    const QSignalBlocker blockGrad(m_gradientEnabled);

    if (m_presetCombo) {
        const QSignalBlocker blockPreset(m_presetCombo);
        const int idx = m_presetCombo->findData(appearance.presetId);
        m_presetCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    }
    if (m_fontSize) {
        m_fontSize->setValue(appearance.fontSize);
        if (m_fontSizeLabel)
            m_fontSizeLabel->setText(QStringLiteral("%1 px").arg(appearance.fontSize));
    }
    if (m_fontFamily) {
        const int idx = m_fontFamily->findData(appearance.fontFamily);
        m_fontFamily->setCurrentIndex(idx >= 0 ? idx : 0);
    }
    if (m_radius) {
        m_radius->setValue(appearance.radius);
        if (m_radiusLabel)
            m_radiusLabel->setText(QStringLiteral("%1 px").arg(appearance.radius));
    }
    if (m_uiScale) {
        m_uiScale->setValue(appearance.uiScale);
        if (m_uiScaleLabel)
            m_uiScaleLabel->setText(QStringLiteral("%1%").arg(appearance.uiScale));
    }
    if (m_brightness) {
        m_brightness->setValue(appearance.brightness);
        if (m_brightnessLabel) {
            const int v = appearance.brightness;
            m_brightnessLabel->setText(v > 0 ? QStringLiteral("+%1").arg(v) : QString::number(v));
        }
    }
    if (m_saturation) {
        m_saturation->setValue(appearance.saturation);
        if (m_saturationLabel) {
            const int v = appearance.saturation;
            m_saturationLabel->setText(v > 0 ? QStringLiteral("+%1").arg(v) : QString::number(v));
        }
    }
    if (m_chatDensity)
        m_chatDensity->setCurrentIndex(m_chatDensity->findData(static_cast<int>(appearance.chatDensity)));
    if (m_syncDiscordAccent)
        m_syncDiscordAccent->setChecked(appearance.syncDiscordAccent);
    if (m_gradientEnabled)
        m_gradientEnabled->setChecked(appearance.gradientEnabled);
    refreshColorSwatches();
    m_syncingAppearance = false;
}

void SettingsDialog::exportTheme()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("Export theme"), QStringLiteral("snapcord-theme.json"),
                                                      tr("Theme JSON (*.json)"));
    if (path.isEmpty())
        return;
    const QString error = Theme::instance().exportToFile(path);
    if (!error.isEmpty())
        QMessageBox::warning(this, tr("Export theme"), error);
}

void SettingsDialog::importTheme()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Import theme"), QString(),
                                                      tr("Theme JSON (*.json)"));
    if (path.isEmpty())
        return;
    const QString error = Theme::instance().importFromFile(path);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, tr("Import theme"), error);
        return;
    }
    refreshAppearanceControls();
}

void SettingsDialog::setCustomAccent(const QColor& color)
{
    Theme::Settings settings = Theme::instance().settings();
    settings.customAccent = color;
    Theme::instance().setSettings(settings);
    refreshColorSwatches();
}

void SettingsDialog::pickColor(const QColor& initial, const QString& title,
                               const std::function<void(QColor)>& onPicked)
{
    // Native portal pickers on Linux often hang the whole Qt app; use Qt's own dialog.
    // DontUseNativeDialog must be set BEFORE setCurrentColor — enabling it recreates the
    // widgets and would wipe the color back to black (empty HTML / HSV 0).
    const QColor start = initial.isValid() ? initial : QColor(Qt::white);
    auto* dialog = new QColorDialog(this);
    dialog->setWindowTitle(title);
    dialog->setOption(QColorDialog::DontUseNativeDialog, true);
    dialog->setCurrentColor(start);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setModal(true);
    connect(dialog, &QColorDialog::colorSelected, this, [onPicked](const QColor& color) {
        if (color.isValid())
            onPicked(color);
    });
    dialog->open();
    dialog->setCurrentColor(start);
}

QString SettingsDialog::presetDisplayName(const QString& id, const QString& fallback)
{
    if (id == QLatin1String("discord"))
        return tr("Discord");
    if (id == QLatin1String("midnight"))
        return tr("Midnight");
    if (id == QLatin1String("amoled"))
        return tr("AMOLED");
    if (id == QLatin1String("ash"))
        return tr("Ash");
    if (id == QLatin1String("catppuccin-mocha"))
        return tr("Catppuccin Mocha");
    if (id == QLatin1String("catppuccin-latte"))
        return tr("Catppuccin Latte");
    if (id == QLatin1String("nord"))
        return tr("Nord");
    if (id == QLatin1String("dracula"))
        return tr("Dracula");
    if (id == QLatin1String("gruvbox"))
        return tr("Gruvbox");
    if (id == QLatin1String("tokyo-night"))
        return tr("Tokyo Night");
    if (id == QLatin1String("rose-pine"))
        return tr("Rosé Pine");
    if (id == QLatin1String("one-dark"))
        return tr("One Dark");
    if (id == QLatin1String("light"))
        return tr("Light");
    return fallback;
}

void SettingsDialog::bindLiveSlider(QSlider* slider, QLabel* label, const std::function<QString(int)>& format)
{
    // Rebuilding the global stylesheet mid-drag steals the mouse from QSlider — only apply when
    // the user releases, or when the value jumps without a press (keyboard / groove click).
    connect(slider, &QSlider::valueChanged, this, [this, slider, label, format](int value) {
        if (label)
            label->setText(format(value));
        if (m_syncingAppearance)
            return;
        if (!slider->isSliderDown())
            applyAppearance();
    });
    connect(slider, &QSlider::sliderReleased, this, [this] {
        if (!m_syncingAppearance)
            applyAppearance();
    });
}

void SettingsDialog::ignoreWheel(QWidget* widget)
{
    if (widget)
        widget->installEventFilter(this);
}

bool SettingsDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Wheel) {
        // Wheel over sliders/combos must scroll the page they are on, not nudge the control.
        QScrollArea* scroll = nullptr;
        for (QObject* object = watched; object && !scroll; object = object->parent())
            scroll = qobject_cast<QScrollArea*>(object);
        if (scroll) {
            if (auto* bar = scroll->verticalScrollBar()) {
                const auto* wheel = static_cast<const QWheelEvent*>(event);
                bar->setValue(bar->value() - wheel->angleDelta().y());
            }
        }
        return true;
    }
    return QDialog::eventFilter(watched, event);
}

void SettingsDialog::applyAppearance()
{
    if (m_syncingAppearance)
        return;
    Theme::Settings settings = Theme::instance().settings();
    if (m_presetCombo)
        settings.presetId = m_presetCombo->currentData().toString();
    if (m_fontSize)
        settings.fontSize = m_fontSize->value();
    if (m_fontFamily)
        settings.fontFamily = m_fontFamily->currentData().toString();
    if (m_radius)
        settings.radius = m_radius->value();
    if (m_uiScale)
        settings.uiScale = m_uiScale->value();
    if (m_brightness)
        settings.brightness = m_brightness->value();
    if (m_saturation)
        settings.saturation = m_saturation->value();
    if (m_chatDensity)
        settings.chatDensity = static_cast<Theme::ChatDensity>(m_chatDensity->currentData().toInt());
    if (m_syncDiscordAccent)
        settings.syncDiscordAccent = m_syncDiscordAccent->isChecked();
    if (m_gradientEnabled)
        settings.gradientEnabled = m_gradientEnabled->isChecked();
    Theme::instance().setSettings(settings);
    refreshColorSwatches();
}

QWidget* SettingsDialog::buildLanguagePage()
{
    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("settingsContent"));
    content->setAttribute(Qt::WA_StyledBackground);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(40, 32, 40, 32);
    layout->setSpacing(12);
    auto* title = new QLabel(tr("Language"));
    title->setObjectName(QStringLiteral("settingsTitle"));
    layout->addWidget(title);
    layout->addSpacing(8);
    layout->addWidget(sectionLabel(tr("Select a language")));

    auto* group = new QButtonGroup(content);
    const QString current = Language::current();
    for (const auto& [code, name] : Language::available()) {
        auto* option = new QRadioButton(name);
        option->setChecked(code == current);
        group->addButton(option);
        layout->addWidget(option);
        connect(option, &QRadioButton::toggled, this, [this, code](bool checked) {
            if (checked)
                changeLanguage(code);
        });
    }
    layout->addStretch();
    return content;
}

QWidget* SettingsDialog::buildAboutPage()
{
    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("settingsContent"));
    content->setAttribute(Qt::WA_StyledBackground);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(40, 32, 40, 32);
    layout->setSpacing(12);

    // Header: icon, name and version.
    auto* icon = new QLabel;
    icon->setPixmap(QIcon(QStringLiteral(":/icons/snapcord.svg")).pixmap(QSize(64, 64), devicePixelRatioF()));
    icon->setFixedSize(64, 64);
    auto* name = new QLabel(QStringLiteral("Snapcord"));
    name->setObjectName(QStringLiteral("settingsTitle"));
    auto* version = new QLabel(tr("Version %1").arg(UpdateChecker::currentVersion()));
    version->setObjectName(QStringLiteral("settingsHint"));
    version->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* titles = new QVBoxLayout;
    titles->setSpacing(2);
    titles->addStretch();
    titles->addWidget(name);
    titles->addWidget(version);
    titles->addStretch();
    auto* header = new QHBoxLayout;
    header->setSpacing(16);
    header->addWidget(icon);
    header->addLayout(titles, 1);
    layout->addLayout(header);
    layout->addSpacing(4);

    auto* description = new QLabel(tr("A lightweight, native and open-source Discord client, made first of all for "
                                      "voice calls."));
    description->setWordWrap(true);
    layout->addWidget(description);

    m_aboutLinks = new QLabel;
    m_aboutLinks->setOpenExternalLinks(true);
    m_aboutLinks->setTextFormat(Qt::RichText);
    layout->addWidget(m_aboutLinks);

    auto* disclaimer = new QLabel(tr("Snapcord is not made by or affiliated with Discord. Third-party clients go "
                                     "against Discord's Terms of Service and may get your account banned; use it "
                                     "at your own risk."));
    disclaimer->setObjectName(QStringLiteral("settingsHint"));
    disclaimer->setWordWrap(true);
    layout->addWidget(disclaimer);

    // Updates.
    layout->addSpacing(12);
    layout->addWidget(sectionLabel(tr("Updates")));
    m_updateStatus = new QLabel;
    m_updateStatus->setWordWrap(true);
    m_checkUpdates = new QPushButton(tr("Check for Updates"));
    m_checkUpdates->setObjectName(QStringLiteral("secondaryButton"));
    m_checkUpdates->setCursor(Qt::PointingHandCursor);
    m_viewUpdate = new QPushButton(tr("View Update"));
    m_viewUpdate->setObjectName(QStringLiteral("brandButton"));
    m_viewUpdate->setCursor(Qt::PointingHandCursor);
    auto* updateRow = new QHBoxLayout;
    updateRow->setSpacing(8);
    updateRow->addWidget(m_updateStatus, 1);
    updateRow->addWidget(m_viewUpdate);
    updateRow->addWidget(m_checkUpdates);
    layout->addLayout(updateRow);

    UpdateChecker& checker = UpdateChecker::instance();
    connect(m_checkUpdates, &QPushButton::clicked, this, [this, &checker] {
        checker.checkNow();
        refreshAboutPage();
    });
    connect(m_viewUpdate, &QPushButton::clicked, this, [this, &checker] {
        auto* dialog = new UpdateDialog(checker.latest(), this);
        dialog->show();
    });
    connect(&checker, &UpdateChecker::checkFinished, this, [this](const QString& error) {
        refreshAboutPage();
        if (!error.isEmpty())
            m_updateStatus->setText(error);
    });

    auto* automatic = new QCheckBox(tr("Check for updates automatically"));
    automatic->setChecked(UpdateChecker::automaticChecks());
    connect(automatic, &QCheckBox::toggled, this, &UpdateChecker::setAutomaticChecks);
    layout->addWidget(option(automatic, tr("Once a day Snapcord asks GitHub for the latest release and tells you "
                                           "when there is a new one. Nothing is downloaded without your click.")));
    auto* prereleases = new QCheckBox(tr("Include pre-releases"));
    prereleases->setChecked(UpdateChecker::includePrereleases());
    connect(prereleases, &QCheckBox::toggled, this, &UpdateChecker::setIncludePrereleases);
    layout->addWidget(option(prereleases, tr("Also tell me about test versions, which may be less stable.")));

    // Contributors (from GitHub) and the libraries that make Snapcord possible.
    layout->addSpacing(12);
    layout->addWidget(sectionLabel(tr("Contributors")));
    m_contributors = new QLabel;
    m_contributors->setWordWrap(true);
    m_contributors->setOpenExternalLinks(true);
    m_contributors->setTextFormat(Qt::RichText);
    layout->addWidget(m_contributors);
    connect(&checker, &UpdateChecker::contributorsLoaded, this, &SettingsDialog::refreshAboutPage);

    layout->addSpacing(12);
    layout->addWidget(sectionLabel(tr("Built with")));
    auto* credits = new QLabel(QStringLiteral("Qt · libdave · mlspp · Opus · miniaudio · RNNoise · SpeexDSP · "
                                              "libsodium · OpenSSL · zlib · QR Code generator"));
    credits->setObjectName(QStringLiteral("settingsHint"));
    credits->setWordWrap(true);
    layout->addWidget(credits);
    layout->addStretch();

    connect(&Theme::instance(), &Theme::changed, this, &SettingsDialog::refreshAboutPage);
    refreshAboutPage();

    auto* scroll = new QScrollArea;
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_aboutPage = scroll;
    return scroll;
}

void SettingsDialog::refreshAboutPage()
{
    const UpdateChecker& checker = UpdateChecker::instance();
    const QString linkColor = Theme::instance().palette().link.name();
    auto link = [&](const QUrl& url, const QString& text) {
        return QStringLiteral("<a style=\"color:%1; text-decoration:none\" href=\"%2\">%3</a>")
            .arg(linkColor, QString::fromUtf8(url.toEncoded()), text.toHtmlEscaped());
    };

    const QString repo = UpdateChecker::repositoryUrl().toString();
    m_aboutLinks->setText(QStringList{
        link(UpdateChecker::repositoryUrl(), tr("Source code")),
        link(QUrl(repo + QStringLiteral("/issues")), tr("Report a problem")),
        link(QUrl(repo + QStringLiteral("/releases")), tr("Releases")),
        link(QUrl(QStringLiteral("https://www.gnu.org/licenses/gpl-3.0.html")), tr("License: GNU GPL v3")),
    }.join(QStringLiteral(" &nbsp;·&nbsp; ")));

    m_checkUpdates->setEnabled(!checker.isChecking());
    m_viewUpdate->setVisible(!checker.isChecking() && checker.updateAvailable());
    if (checker.isChecking()) {
        m_updateStatus->setText(tr("Checking for updates…"));
    } else if (checker.updateAvailable()) {
        m_updateStatus->setText(tr("Version %1 is available.").arg(checker.latest().version));
    } else if (const QDateTime last = UpdateChecker::lastCheck(); last.isValid()) {
        m_updateStatus->setText(tr("You are on the latest version. Last checked: %1.")
                                    .arg(QLocale().toString(last.toLocalTime(), QLocale::ShortFormat)));
    } else {
        m_updateStatus->setText(tr("Not checked yet."));
    }

    const auto& people = checker.contributors();
    if (people.isEmpty()) {
        m_contributors->setText(tr("See everyone who helped on %1.")
                                    .arg(link(QUrl(repo + QStringLiteral("/graphs/contributors")), QStringLiteral("GitHub"))));
    } else {
        QStringList names;
        for (const UpdateChecker::Contributor& person : people)
            names.append(link(person.profileUrl, person.login));
        m_contributors->setText(names.join(QStringLiteral(" &nbsp;·&nbsp; ")));
    }
}

void SettingsDialog::apply()
{
    m_settings.inputDevice = m_inputDevice->currentData().toString();
    m_settings.outputDevice = m_outputDevice->currentData().toString();
    m_settings.inputVolume = m_inputVolume->value() / 100.0f;
    m_settings.outputVolume = m_outputVolume->value() / 100.0f;
    m_settings.inputMode = m_pushToTalk->isChecked() ? VoiceSettings::InputMode::PushToTalk
                                                     : VoiceSettings::InputMode::VoiceActivity;
    m_settings.activationThresholdDb = m_meter->threshold();
    m_settings.pushToTalkKey = m_keybind->key();
    m_settings.pushToTalkReleaseMs = m_releaseDelay->value();
    m_settings.automaticSensitivity = m_automaticSensitivity->isChecked();
    m_settings.noiseSuppression = m_noiseSuppression->isChecked();
    m_settings.echoCancellation = m_echoCancellation->isChecked();
    m_settings.automaticGainControl = m_automaticGainControl->isChecked();
    m_settings.soundEffects = m_soundEffects->isChecked();
    m_settings.participantMuteSounds = m_participantMuteSounds->isChecked();
    m_voice->applySettings(m_settings);
}

void SettingsDialog::updateModeWidgets()
{
    const bool pushToTalk = m_pushToTalk->isChecked();
    m_pushToTalkGroup->setVisible(pushToTalk);
    m_automaticSensitivity->setVisible(!pushToTalk);
    // With automatic sensitivity, voice detection decides; the meter only shows the level.
    m_meter->setThresholdVisible(!pushToTalk && !m_automaticSensitivity->isChecked());
}

void SettingsDialog::applyDialogChrome()
{
    // Fusion QDialog keeps a light Window brush across stylesheet swaps; paint + palette
    // keep the modal on the active theme.
    const Theme::Palette& c = Theme::instance().palette();
    auto tint = [&](QWidget* widget, const QColor& background) {
        if (!widget)
            return;
        QPalette pal = widget->palette();
        pal.setColor(QPalette::Window, background);
        pal.setColor(QPalette::Base, background);
        pal.setColor(QPalette::AlternateBase, c.bg1);
        pal.setColor(QPalette::Text, c.text);
        pal.setColor(QPalette::WindowText, c.text);
        pal.setColor(QPalette::Button, c.button);
        pal.setColor(QPalette::ButtonText, c.textBright);
        pal.setColor(QPalette::Highlight, c.selected);
        pal.setColor(QPalette::HighlightedText, c.textBright);
        widget->setPalette(pal);
        widget->setAutoFillBackground(true);
    };

    tint(this, c.bg2);
    tint(m_settingsSide, c.bg1);
    tint(m_settingsPages, c.bg2);

    for (QScrollArea* scroll : findChildren<QScrollArea*>()) {
        tint(scroll, c.bg2);
        tint(scroll->viewport(), c.bg2);
        if (QWidget* inner = scroll->widget())
            tint(inner, c.bg2);
    }
    update();
}

void SettingsDialog::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), Theme::instance().palette().bg2);
}

void SettingsDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    applyDialogChrome();
    // Outside of a call, open the microphone just for the level meter while the dialog is visible.
    if (m_voice->state() != VoiceConnection::State::Connected)
        startMicTest();
    m_meterTimer->start();
}

void SettingsDialog::hideEvent(QHideEvent* event)
{
    m_meterTimer->stop();
    // Stop capture before the dialog finishes hiding; otherwise PipeWire can block the UI thread
    // while the modal event loop is still unwinding.
    stopMicTest();
    QDialog::hideEvent(event);
}

void SettingsDialog::startMicTest()
{
    if (!m_testAudio)
        m_testAudio = std::make_unique<AudioEngine>();
    // Capture only the atomic level — never touch widgets from the audio thread.
    m_testAudio->startCapture(m_settings.inputDevice, [this](const float* samples, int count) {
        const float gain = m_settings.inputVolume;
        double energy = 0;
        for (int i = 0; i < count; ++i)
            energy += double(samples[i] * gain) * (samples[i] * gain);
        const double rms = count > 0 ? std::sqrt(energy / count) : 0.0;
        m_testLevelDb.store(rms > 1e-9 ? static_cast<float>(20.0 * std::log10(rms)) : -100.0f);
    });
}

void SettingsDialog::stopMicTest()
{
    if (!m_testAudio)
        return;
    m_testAudio->stopCapture();
    m_testAudio.reset();
    m_testLevelDb.store(-100.0f);
}

void SettingsDialog::changeLanguage(const QString& code)
{
    if (code == Language::current())
        return;
    Language::setCurrent(code);

    QMessageBox box(this);
    box.setWindowTitle(tr("Language"));
    box.setText(tr("Restart Snapcord to apply the new language."));
    QPushButton* restartNow = box.addButton(tr("Restart Now"), QMessageBox::AcceptRole);
    box.addButton(tr("Later"), QMessageBox::RejectRole);
    box.exec();

    if (box.clickedButton() == restartNow) {
        QProcess::startDetached(QCoreApplication::applicationFilePath(), QCoreApplication::arguments().mid(1));
        QCoreApplication::quit();
    }
}
