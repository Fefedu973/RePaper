#include "ClipboardBridge.h"
#include <QClipboard>
#include <QGuiApplication>
#include <QStandardPaths>

namespace repaper {
ClipboardBridge::ClipboardBridge(QObject *parent):QObject(parent) {
    m_timeout.setSingleShot(true);m_timeout.setInterval(5000);
    connect(&m_timeout,&QTimer::timeout,this,&ClipboardBridge::fail);
    connect(&m_process,&QProcess::readyReadStandardOutput,this,[this]{
        m_output+=m_process.readAllStandardOutput();if(m_output.size()>1024*1024)fail();
    });
    connect(&m_process,&QProcess::readyReadStandardError,this,[this]{m_process.readAllStandardError();});
    connect(&m_process,&QProcess::errorOccurred,this,[this]{fail();});
    connect(&m_process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this](int code,QProcess::ExitStatus status){
        if(!m_pending)return;
        m_output+=m_process.readAllStandardOutput();m_timeout.stop();m_pending=false;
        if(code==0&&status==QProcess::NormalExit&&m_output.size()<=1024*1024){const auto text=QString::fromUtf8(m_output);m_output.clear();emit ready(text);}
        else {m_output.clear();emit failed();}
    });
}
void ClipboardBridge::fail() {
    if(!m_pending)return;m_pending=false;m_timeout.stop();m_output.clear();
    if(m_process.state()!=QProcess::NotRunning)m_process.kill();emit failed();
}
void ClipboardBridge::request() {
    if(m_pending)return;
    if(qEnvironmentVariable("REPAPER_PC_EMULATOR")!="1") {
        emit ready(QGuiApplication::clipboard()->text());return;
    }
#ifdef Q_OS_WIN
    emit ready(QGuiApplication::clipboard()->text());
#else
    const auto executable=QStandardPaths::findExecutable("powershell.exe");
    if(executable.isEmpty()){emit failed();return;}
    // Constant command: clipboard data never enters shell arguments. PowerShell
    // writes UTF-8 to the anonymous stdout pipe; stderr is discarded above.
    const QString script=QStringLiteral("$ErrorActionPreference='Stop'; [Console]::OutputEncoding=New-Object System.Text.UTF8Encoding($false); $v=Get-Clipboard -Raw; if($null -ne $v){if($v.Length -gt 262144){throw 'Clipboard too large'}; [Console]::Write($v)}");
    m_pending=true;m_output.clear();m_timeout.start();
    m_process.start(executable,{"-NoLogo","-NoProfile","-NonInteractive","-WindowStyle","Hidden","-Command",script});
#endif
}
}
