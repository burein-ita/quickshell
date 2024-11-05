#include "paths.hpp"
#include <cerrno>
#include <cstdio>
#include <utility>

#include <fcntl.h>
#include <qcontainerfwd.h>
#include <qdatastream.h>
#include <qdir.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qstandardpaths.h>
#include <qtenvironmentvariables.h>
#include <unistd.h>

#include "instanceinfo.hpp"

Q_LOGGING_CATEGORY(logPaths, "quickshell.paths", QtWarningMsg);

QsPaths* QsPaths::instance() {
	static auto* instance = new QsPaths(); // NOLINT
	return instance;
}

void QsPaths::init(QString shellId, QString pathId) {
	auto* instance = QsPaths::instance();
	instance->shellId = std::move(shellId);
	instance->pathId = std::move(pathId);
}

QDir QsPaths::crashDir(const QString& id) {
	auto dir = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
	dir = QDir(dir.filePath("crashes"));
	dir = QDir(dir.filePath(id));

	return dir;
}

QString QsPaths::basePath(const QString& id) {
	auto path = QsPaths::instance()->baseRunDir()->filePath("by-id");
	path = QDir(path).filePath(id);
	return path;
}

QString QsPaths::ipcPath(const QString& id) {
	return QDir(QsPaths::basePath(id)).filePath("ipc.sock");
}

QDir* QsPaths::cacheDir() {
	if (this->cacheState == DirState::Unknown) {
		auto dir = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
		dir = QDir(dir.filePath(this->shellId));
		this->mCacheDir = dir;

		qCDebug(logPaths) << "Initialized cache path:" << dir.path();

		if (!dir.mkpath(".")) {
			qCCritical(logPaths) << "Could not create cache directory at" << dir.path();

			this->cacheState = DirState::Failed;
		} else {
			this->cacheState = DirState::Ready;
		}
	}

	if (this->cacheState == DirState::Failed) return nullptr;
	else return &this->mCacheDir;
}

QDir* QsPaths::baseRunDir() {
	if (this->baseRunState == DirState::Unknown) {
		auto runtimeDir = qEnvironmentVariable("XDG_RUNTIME_DIR");
		if (runtimeDir.isEmpty()) {
			runtimeDir = QString("/run/user/$1").arg(getuid());
			qCInfo(logPaths) << "XDG_RUNTIME_DIR was not set, defaulting to" << runtimeDir;
		}

		this->mBaseRunDir = QDir(runtimeDir);
		this->mBaseRunDir = QDir(this->mBaseRunDir.filePath("quickshell"));
		qCDebug(logPaths) << "Initialized base runtime path:" << this->mBaseRunDir.path();

		if (!this->mBaseRunDir.mkpath(".")) {
			qCCritical(logPaths) << "Could not create base runtime directory at"
			                     << this->mBaseRunDir.path();

			this->baseRunState = DirState::Failed;
		} else {
			this->baseRunState = DirState::Ready;
		}
	}

	if (this->baseRunState == DirState::Failed) return nullptr;
	else return &this->mBaseRunDir;
}

QDir* QsPaths::shellRunDir() {
	if (this->shellRunState == DirState::Unknown) {
		if (auto* baseRunDir = this->baseRunDir()) {
			this->mShellRunDir = QDir(baseRunDir->filePath("by-shell"));
			this->mShellRunDir = QDir(this->mShellRunDir.filePath(this->shellId));

			qCDebug(logPaths) << "Initialized runtime path:" << this->mShellRunDir.path();

			if (!this->mShellRunDir.mkpath(".")) {
				qCCritical(logPaths) << "Could not create runtime directory at"
				                     << this->mShellRunDir.path();
				this->shellRunState = DirState::Failed;
			} else {
				this->shellRunState = DirState::Ready;
			}
		} else {
			qCCritical(logPaths) << "Could not create shell runtime path as it was not possible to "
			                        "create the base runtime path.";

			this->shellRunState = DirState::Failed;
		}
	}

	if (this->shellRunState == DirState::Failed) return nullptr;
	else return &this->mShellRunDir;
}

QDir* QsPaths::instanceRunDir() {
	if (this->instanceRunState == DirState::Unknown) {
		auto* runDir = this->baseRunDir();

		if (!runDir) {
			qCCritical(logPaths) << "Cannot create instance runtime directory as main runtim directory "
			                        "could not be created.";
			this->instanceRunState = DirState::Failed;
		} else {
			auto byIdDir = QDir(runDir->filePath("by-id"));

			this->mInstanceRunDir = byIdDir.filePath(InstanceInfo::CURRENT.instanceId);

			qCDebug(logPaths) << "Initialized instance runtime path:" << this->mInstanceRunDir.path();

			if (!this->mInstanceRunDir.mkpath(".")) {
				qCCritical(logPaths) << "Could not create instance runtime directory at"
				                     << this->mInstanceRunDir.path();
				this->instanceRunState = DirState::Failed;
			} else {
				this->instanceRunState = DirState::Ready;
			}
		}
	}

	if (this->shellRunState == DirState::Failed) return nullptr;
	else return &this->mInstanceRunDir;
}

void QsPaths::linkRunDir() {
	if (auto* runDir = this->instanceRunDir()) {
		auto pidDir = QDir(this->baseRunDir()->filePath("by-pid"));
		auto* shellDir = this->shellRunDir();

		if (!shellDir) {
			qCCritical(logPaths
			) << "Could not create by-id symlink as the shell runtime path could not be created.";
		} else {
			auto shellPath = shellDir->filePath(runDir->dirName());

			QFile::remove(shellPath);
			auto r =
			    symlinkat(runDir->filesystemCanonicalPath().c_str(), 0, shellPath.toStdString().c_str());

			if (r != 0) {
				qCCritical(logPaths).nospace()
				    << "Could not create id symlink to " << runDir->path() << " at " << shellPath
				    << " with error code " << errno << ": " << qt_error_string();
			} else {
				qCDebug(logPaths) << "Created shellid symlink" << shellPath << "to instance runtime path"
				                  << runDir->path();
			}
		}

		if (!pidDir.mkpath(".")) {
			qCCritical(logPaths) << "Could not create PID symlink directory.";
		} else {
			auto pidPath = pidDir.filePath(QString::number(getpid()));

			QFile::remove(pidPath);
			auto r =
			    symlinkat(runDir->filesystemCanonicalPath().c_str(), 0, pidPath.toStdString().c_str());

			if (r != 0) {
				qCCritical(logPaths).nospace()
				    << "Could not create PID symlink to " << runDir->path() << " at " << pidPath
				    << " with error code " << errno << ": " << qt_error_string();
			} else {
				qCDebug(logPaths) << "Created PID symlink" << pidPath << "to instance runtime path"
				                  << runDir->path();
			}
		}
	} else {
		qCCritical(logPaths) << "Could not create PID symlink to runtime directory, as the runtime "
		                        "directory could not be created.";
	}
}

void QsPaths::linkPathDir() {
	if (auto* runDir = this->shellRunDir()) {
		auto pathDir = QDir(this->baseRunDir()->filePath("by-path"));

		if (!pathDir.mkpath(".")) {
			qCCritical(logPaths) << "Could not create path symlink directory.";
			return;
		}

		auto linkPath = pathDir.filePath(this->pathId);

		QFile::remove(linkPath);
		auto r =
		    symlinkat(runDir->filesystemCanonicalPath().c_str(), 0, linkPath.toStdString().c_str());

		if (r != 0) {
			qCCritical(logPaths).nospace()
			    << "Could not create path symlink to " << runDir->path() << " at " << linkPath
			    << " with error code " << errno << ": " << qt_error_string();
		} else {
			qCDebug(logPaths) << "Created path symlink" << linkPath << "to shell runtime path"
			                  << runDir->path();
		}
	} else {
		qCCritical(logPaths) << "Could not create path symlink to shell runtime directory, as the "
		                        "shell runtime directory could not be created.";
	}
}

void QsPaths::createLock() {
	if (auto* runDir = this->instanceRunDir()) {
		auto path = runDir->filePath("instance.lock");
		auto* file = new QFile(path); // leaked

		if (!file->open(QFile::ReadWrite | QFile::Truncate)) {
			qCCritical(logPaths) << "Could not create instance lock at" << path;
			return;
		}

		auto lock = flock {
		    .l_type = F_WRLCK,
		    .l_whence = SEEK_SET,
		    .l_start = 0,
		    .l_len = 0,
		    .l_pid = 0,
		};

		if (fcntl(file->handle(), F_SETLK, &lock) != 0) { // NOLINT
			qCCritical(logPaths).nospace() << "Could not lock instance lock at " << path
			                               << " with error code " << errno << ": " << qt_error_string();
		} else {
			auto stream = QDataStream(file);
			stream << InstanceInfo::CURRENT;
			file->flush();
			qCDebug(logPaths) << "Created instance lock at" << path;
		}
	} else {
		qCCritical(logPaths
		) << "Could not create instance lock, as the instance runtime directory could not be created.";
	}
}

bool QsPaths::checkLock(const QString& path, InstanceLockInfo* info) {
	auto file = QFile(QDir(path).filePath("instance.lock"));
	if (!file.open(QFile::ReadOnly)) return false;

	auto lock = flock {
	    .l_type = F_WRLCK,
	    .l_whence = SEEK_SET,
	    .l_start = 0,
	    .l_len = 0,
	    .l_pid = 0,
	};

	fcntl(file.handle(), F_GETLK, &lock); // NOLINT
	if (lock.l_type == F_UNLCK) return false;

	if (info) {
		info->pid = lock.l_pid;

		auto stream = QDataStream(&file);
		stream >> info->instance;
	}

	return true;
}

QVector<InstanceLockInfo> QsPaths::collectInstances(const QString& path) {
	qCDebug(logPaths) << "Collecting instances from" << path;
	auto instances = QVector<InstanceLockInfo>();
	auto dir = QDir(path);

	InstanceLockInfo info;
	for (auto& entry: dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
		auto path = dir.filePath(entry);

		if (QsPaths::checkLock(path, &info)) {
			qCDebug(logPaths).nospace() << "Found live instance " << info.instance.instanceId << " (pid "
			                            << info.pid << ") at " << path;

			instances.push_back(info);
		} else {
			qCDebug(logPaths) << "Skipped dead instance at" << path;
		}
	}

	return instances;
}
