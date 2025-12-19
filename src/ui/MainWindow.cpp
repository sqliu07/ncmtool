#include "MainWindow.h"
#include "ui_MainWindow.h"
#include "Worker.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QFileInfo>
#include <QProcess>
#include <QLibrary>
#include <QStandardItemModel>
#include <QProgressDialog>
#include <QThread>
#include <QMimeData>
#include <QUrl>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QSettings>

typedef void* (__cdecl *CreateCryptFunc)(const char*);
typedef int   (__cdecl *DumpFunc)(void*, const char*);
typedef void  (__cdecl *FixFunc)(void*);
typedef void  (__cdecl *DestroyFunc)(void*);

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    connect(ui->btnRemoveSelected, &QPushButton::clicked, this, [=]() {
        QList<QListWidgetItem*> items = ui->listFiles->selectedItems();
        for (QListWidgetItem* item : items) {
            selectedFiles.removeAll(item->text());
            delete item;
        }
        // 更新文件数量显示
        if (selectedFiles.isEmpty()) {
            ui->labelFile->setText("未选择文件");
        } else {
            ui->labelFile->setText(QString("已选择 %1 个 文件").arg(selectedFiles.size()));
        }
    });
    connect(ui->btnSelectFolder, &QPushButton::clicked, this, &MainWindow::on_btnSelectFolder_clicked);
    QSettings settings("ncmtool", "config");
    QString lastOut = settings.value("lastOutputDir").toString();
    if (!lastOut.isEmpty()) {
        outputDir = lastOut;
        ui->labelOutput->setText("默认输出路径: " + outputDir);
    }
    setAcceptDrops(true);
    ui->comboFormat->addItems({"保持原始格式", "转为 MP3", "转为 FLAC"});
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(ui->comboFormat->model());
    if (model && model->item(2)) {
        model->item(2)->setToolTip("MP3 是有损格式，转为 FLAC 并不会提升音质。");
    }
    ui->textLog->setReadOnly(true);
    ui->listFiles->setSelectionMode(QAbstractItemView::ExtendedSelection);
    this->resize(800, 600);
}

MainWindow::~MainWindow() {
    delete ui;
}

void MainWindow::addExternalFile(const QString &path) {
    if (!selectedFiles.contains(path)) {
        selectedFiles.append(path);
        ui->listFiles->addItem(path);
        ui->labelFile->setText(QString("已选择 %1 个 文件").arg(selectedFiles.size()));
    }
}
void MainWindow::on_btnSelectFile_clicked() {
    QSettings settings("ncmtool", "config");
    QString defaultDir = settings.value("lastInputDir", QDir::homePath()).toString();

    QStringList files = QFileDialog::getOpenFileNames(this, "选择 .ncm 文件", defaultDir, "NCM Files (*.ncm)");
    if (files.isEmpty()) return;

    selectedFiles = files;
    ui->listFiles->clear();
    for (const QString &f : selectedFiles) {
        ui->listFiles->addItem(f);
    }

    ui->labelFile->setText(QString("已选择 %1 个 文件").arg(selectedFiles.size()));

    // 记住路径
    QFileInfo fi(selectedFiles.first());
    settings.setValue("lastInputDir", fi.absolutePath());
}

void MainWindow::on_btnSelectFolder_clicked() {
    QSettings settings("ncmtool", "config");
    QString defaultDir = settings.value("lastInputDir", QDir::homePath()).toString();
    QString folder = QFileDialog::getExistingDirectory(this, "选择包含 .ncm 文件的文件夹", defaultDir);
    if (folder.isEmpty()) return;

    QDir dir(folder);
    QFileInfoList fileList = dir.entryInfoList(QStringList("*.ncm"), QDir::Files | QDir::NoSymLinks);

    int added = 0;
    for (const QFileInfo &fileInfo : fileList) {
        QString path = fileInfo.absoluteFilePath();
        if (!selectedFiles.contains(path)) {
            selectedFiles.append(path);
            ui->listFiles->addItem(path);
            added++;
        }
    }

    if (added == 0) {
        QMessageBox::information(this, "没有新文件", "该文件夹下没有可用的 .ncm 文件或已全部添加。");
    }

    ui->labelFile->setText(QString("已选择 %1 个 文件").arg(selectedFiles.size()));
    settings.setValue("lastInputDir", folder);
}

void MainWindow::on_btnSelectOutput_clicked() {
    QSettings settings("ncmtool", "config");
    QString defaultDir = settings.value("lastOutputDir", QDir::homePath()).toString();

    QString dir = QFileDialog::getExistingDirectory(this, "选择输出目录", defaultDir);
    if (dir.isEmpty()) return;

    outputDir = dir;
    ui->labelOutput->setText(dir);

    // 保存路径
    settings.setValue("lastOutputDir", dir);
}

void MainWindow::on_btnStart_clicked() {
    if (selectedFiles.isEmpty()) {
        QMessageBox::warning(this, "提示", "请先选择文件");
        return;
    }

    QString actualOutputDir = outputDir;
    if (actualOutputDir.isEmpty()) {
        actualOutputDir = QFileInfo(selectedFiles.first()).absolutePath();
        ui->labelOutput->setText("默认路径: " + actualOutputDir);
    }

    ui->textLog->clear();
    ui->progressBar->setValue(0);
    ui->progressBar->setMaximum(selectedFiles.size());
    ui->btnCancel->setVisible(true);
    ui->btnCancel->setEnabled(true);
    bool deleteAfter = ui->checkDeleteOriginal->isChecked();
    worker = new Worker(selectedFiles, actualOutputDir, ui->comboFormat->currentText(),  deleteAfter);
    thread = new QThread(this);

    worker->moveToThread(thread);

    connect(thread, &QThread::started, worker, &Worker::start);
    connect(worker, &Worker::log, this, [=](const QString &msg) {
        ui->textLog->append(msg);
    });
    connect(worker, &Worker::progress, this, [=](int current) {
        ui->progressBar->setValue(current);
    });

    connect(worker, &Worker::done, this, [=]() {
        ui->textLog->append("✅ 所有文件处理完成！");
        ui->btnCancel->setVisible(false);
        thread->quit();
        thread->wait();
        worker->deleteLater();
        thread->deleteLater();
        worker = nullptr;
        thread = nullptr;
    });

    connect(worker, &Worker::cancelled, this, [=]() {
        ui->textLog->append("🚫 任务已取消");
        ui->btnCancel->setVisible(false);
        thread->quit();
        thread->wait();
        worker->deleteLater();
        thread->deleteLater();
        worker = nullptr;
        thread = nullptr;
    });

    connect(ui->btnCancel, &QPushButton::clicked, this, [=]() {
        if (worker) {
            ui->btnCancel->setEnabled(false);
            worker->cancel();
            ui->textLog->append("⏹ 正在尝试取消...");
        }
    });

    thread->start();
}


void MainWindow::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent *event) {
    QList<QUrl> urls = event->mimeData()->urls();
    for (const QUrl &url : urls) {
        QString filePath = url.toLocalFile();
        if (filePath.endsWith(".ncm", Qt::CaseInsensitive)) {
            if (!selectedFiles.contains(filePath)) {
                selectedFiles.append(filePath);
                ui->listFiles->addItem(filePath);
            }
        }
    }

    if (selectedFiles.isEmpty()) {
        ui->labelFile->setText("未选择文件");
    } else {
        ui->labelFile->setText(QString("已选择 %1 个 文件").arg(selectedFiles.size()));
    }
}
bool MainWindow::downloadFfmpegToLib() {
    return false;
}
