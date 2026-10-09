#pragma once

#include "Theme.h"
#include "voice/VoiceSettings.h"

#include <QDialog>
#include <QMap>
#include <QPushButton>
#include <QString>
#include <QVector>
#include <QWidget>

#include <atomic>
#include <functional>
#include <memory>

class AudioEngine;
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QEvent;
class QLabel;
class QRadioButton;
class QPaintEvent;
class QScrollArea;
class QSlider;
class QStackedWidget;
class QTimer;
class VoiceController;

// Microphone level bar that doubles as the voice activity sensitivity control (drag to set the threshold).
class LevelMeter : public QWidget
{
    Q_OBJECT

public:
    explicit LevelMeter(QWidget* parent = nullptr);

    void setLevel(float db);
    void setThreshold(float db);
    float threshold() const { return m_threshold; }
    void setThresholdVisible(bool visible);

    QSize sizeHint() const override { return {300, 24}; }

signals:
    void thresholdChanged(float db);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    float dbAt(int x) const;

    float m_level = -100.0f;
    float m_threshold = -50.0f;
    bool m_thresholdVisible = true;
};

// Button that records the next key or mouse button press as the push-to-talk key.
class KeybindButton : public QPushButton
{
    Q_OBJECT

public:
    explicit KeybindButton(QWidget* parent = nullptr);

    void setKey(int nativeKey);
    int key() const { return m_key; }

signals:
    void keyChanged(int nativeKey);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    void stopRecording();
    void updateText();

    int m_key = 0;
    bool m_recording = false;
};

// Solid color chip for Appearance. Painted directly so global QSS cannot wipe the fill.
class ColorSwatch : public QPushButton
{
    Q_OBJECT

public:
    explicit ColorSwatch(QWidget* parent = nullptr);

    void setSwatchColor(const QColor& color);
    QColor swatchColor() const { return m_color; }
    void setSelectedSwatch(bool selected);
    bool isSelectedSwatch() const { return m_selected; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QColor m_color{Qt::black};
    bool m_selected = false;
};

// Clickable layout map: rail / sidebar / chat / accent stripe.
class LayoutStudio : public QWidget
{
    Q_OBJECT

public:
    explicit LayoutStudio(QWidget* parent = nullptr);

    QSize sizeHint() const override { return {420, 140}; }
    QSize minimumSizeHint() const override { return {280, 120}; }

signals:
    void regionClicked(const QString& tokenId);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QString hitTest(const QPoint& pos) const;
    QString m_hover;
};

class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(VoiceController* voice, QWidget* parent = nullptr);
    ~SettingsDialog() override;

signals:
    void logoutRequested();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QWidget* buildVoicePage();
    QWidget* buildAppearancePage();
    QWidget* buildSoundsPage();
    QWidget* buildNotificationsPage();
    QWidget* buildActivityPage();
    QWidget* buildLanguagePage();
    QWidget* buildAboutPage();
    void refreshAboutPage();
    void apply();
    void applyAppearance();
    void applyDialogChrome();
    void updateModeWidgets();
    void startMicTest();
    void stopMicTest();
    void changeLanguage(const QString& code);
    void refreshColorSwatches();
    void refreshAppearanceControls();
    void setCustomAccent(const QColor& color);
    void pickColor(const QColor& initial, const QString& title, const std::function<void(QColor)>& onPicked);
    void exportTheme();
    void importTheme();
    void bindLiveSlider(QSlider* slider, QLabel* label, const std::function<QString(int)>& format);
    void ignoreWheel(QWidget* widget);
    static QString presetDisplayName(const QString& id, const QString& fallback);

    VoiceController* m_voice;
    VoiceSettings m_settings;

    QComboBox* m_inputDevice = nullptr;
    QComboBox* m_outputDevice = nullptr;
    QSlider* m_inputVolume = nullptr;
    QSlider* m_outputVolume = nullptr;
    QRadioButton* m_voiceActivity = nullptr;
    QRadioButton* m_pushToTalk = nullptr;
    LevelMeter* m_meter = nullptr;
    QWidget* m_sensitivityGroup = nullptr;
    QWidget* m_pushToTalkGroup = nullptr;
    KeybindButton* m_keybind = nullptr;
    QSlider* m_releaseDelay = nullptr;
    QLabel* m_releaseDelayLabel = nullptr;
    QCheckBox* m_automaticSensitivity = nullptr;
    QCheckBox* m_noiseSuppression = nullptr;
    QCheckBox* m_echoCancellation = nullptr;
    QCheckBox* m_automaticGainControl = nullptr;
    QCheckBox* m_soundEffects = nullptr;
    QCheckBox* m_participantMuteSounds = nullptr;

    QSlider* m_fontSize = nullptr;
    QLabel* m_fontSizeLabel = nullptr;
    QComboBox* m_fontFamily = nullptr;
    QComboBox* m_chatDensity = nullptr;
    QSlider* m_radius = nullptr;
    QLabel* m_radiusLabel = nullptr;
    QSlider* m_uiScale = nullptr;
    QLabel* m_uiScaleLabel = nullptr;
    QSlider* m_brightness = nullptr;
    QLabel* m_brightnessLabel = nullptr;
    QSlider* m_saturation = nullptr;
    QLabel* m_saturationLabel = nullptr;
    LayoutStudio* m_layoutStudio = nullptr;
    QLabel* m_customizedLabel = nullptr;
    QScrollArea* m_appearanceScroll = nullptr;
    QWidget* m_settingsSide = nullptr;
    QStackedWidget* m_settingsPages = nullptr;
    bool m_syncingAppearance = false;
    ColorSwatch* m_accentSwatch = nullptr;
    ColorSwatch* m_bg0Swatch = nullptr;
    ColorSwatch* m_bg1Swatch = nullptr;
    ColorSwatch* m_bg2Swatch = nullptr;
    ColorSwatch* m_profilePrimarySwatch = nullptr;
    ColorSwatch* m_profileAccentSwatch = nullptr;
    ColorSwatch* m_gradientTopSwatch = nullptr;
    ColorSwatch* m_gradientBottomSwatch = nullptr;
    QCheckBox* m_syncDiscordAccent = nullptr;
    QCheckBox* m_gradientEnabled = nullptr;
    QComboBox* m_presetCombo = nullptr;
    QVector<ColorSwatch*> m_accentChips;
    QMap<QString, ColorSwatch*> m_tokenSwatches;

    QWidget* m_aboutPage = nullptr;
    QLabel* m_aboutLinks = nullptr;
    QLabel* m_updateStatus = nullptr;
    QPushButton* m_checkUpdates = nullptr;
    QPushButton* m_viewUpdate = nullptr;
    QLabel* m_contributors = nullptr;

    QTimer* m_meterTimer;
    std::unique_ptr<AudioEngine> m_testAudio;
    std::atomic<float> m_testLevelDb{-100.0f};
};
