#include "core/UpdateChecker.h"

#include <QTest>

class UpdateCheckerTest : public QObject
{
    Q_OBJECT

private slots:
    void compareVersions_data()
    {
        QTest::addColumn<QString>("a");
        QTest::addColumn<QString>("b");
        QTest::addColumn<int>("expected");

        QTest::newRow("equal") << "0.2.0" << "0.2.0" << 0;
        QTest::newRow("v prefix") << "v0.2.0" << "0.2.0" << 0;
        QTest::newRow("patch") << "0.2.1" << "0.2.0" << 1;
        QTest::newRow("minor beats patch") << "0.3.0" << "0.2.9" << 1;
        QTest::newRow("numeric, not text") << "0.10.0" << "0.9.0" << 1;
        QTest::newRow("missing part is zero") << "1.0" << "1.0.0" << 0;
        QTest::newRow("older") << "0.1.9" << "v0.2.0" << -1;
        QTest::newRow("pre-release before release") << "0.3.0-beta.1" << "0.3.0" << -1;
        QTest::newRow("pre-release after older release") << "0.3.0-beta.1" << "0.2.0" << 1;
        QTest::newRow("pre-release numbers") << "0.3.0-beta.10" << "0.3.0-beta.2" << 1;
        QTest::newRow("pre-release text") << "0.3.0-rc.1" << "0.3.0-beta.5" << 1;
        QTest::newRow("longer pre-release") << "0.3.0-beta.1" << "0.3.0-beta" << 1;
        QTest::newRow("build metadata ignored") << "0.3.0+abc" << "0.3.0" << 0;
    }

    void compareVersions()
    {
        QFETCH(QString, a);
        QFETCH(QString, b);
        QFETCH(int, expected);
        QCOMPARE(UpdateChecker::compareVersions(a, b), expected);
        QCOMPARE(UpdateChecker::compareVersions(b, a), -expected);
    }
};

QTEST_GUILESS_MAIN(UpdateCheckerTest)
#include "UpdateCheckerTest.moc"
