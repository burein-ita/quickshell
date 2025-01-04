#include "proxywindow.hpp"

#include <private/qquickwindow_p.h>
#include <qevent.h>
#include <qnamespace.h>
#include <qobject.h>
#include <qqmlcontext.h>
#include <qqmlengine.h>
#include <qqmlinfo.h>
#include <qqmllist.h>
#include <qquickitem.h>
#include <qquickwindow.h>
#include <qregion.h>
#include <qsurfaceformat.h>
#include <qtenvironmentvariables.h>
#include <qtmetamacros.h>
#include <qtypes.h>
#include <qvariant.h>
#include <qwindow.h>

#include "../core/generation.hpp"
#include "../core/qmlglobal.hpp"
#include "../core/qmlscreen.hpp"
#include "../core/region.hpp"
#include "../core/reload.hpp"
#include "../debug/lint.hpp"
#include "windowinterface.hpp"

ProxyWindowBase::ProxyWindowBase(QObject* parent)
    : Reloadable(parent)
    , mContentItem(new QQuickItem()) {
	QQmlEngine::setObjectOwnership(this->mContentItem, QQmlEngine::CppOwnership);
	this->mContentItem->setParent(this);

	// clang-format off
	QObject::connect(this, &ProxyWindowBase::widthChanged, this, &ProxyWindowBase::onWidthChanged);
	QObject::connect(this, &ProxyWindowBase::heightChanged, this, &ProxyWindowBase::onHeightChanged);

	QObject::connect(this, &ProxyWindowBase::maskChanged, this, &ProxyWindowBase::onMaskChanged);
	QObject::connect(this, &ProxyWindowBase::widthChanged, this, &ProxyWindowBase::onMaskChanged);
	QObject::connect(this, &ProxyWindowBase::heightChanged, this, &ProxyWindowBase::onMaskChanged);

	QObject::connect(this, &ProxyWindowBase::xChanged, this, &ProxyWindowBase::windowTransformChanged);
	QObject::connect(this, &ProxyWindowBase::yChanged, this, &ProxyWindowBase::windowTransformChanged);
	QObject::connect(this, &ProxyWindowBase::widthChanged, this, &ProxyWindowBase::windowTransformChanged);
	QObject::connect(this, &ProxyWindowBase::heightChanged, this, &ProxyWindowBase::windowTransformChanged);
	QObject::connect(this, &ProxyWindowBase::backerVisibilityChanged, this, &ProxyWindowBase::windowTransformChanged);
	// clang-format on
}

ProxyWindowBase::~ProxyWindowBase() { this->deleteWindow(true); }

void ProxyWindowBase::onReload(QObject* oldInstance) {
	this->window = this->retrieveWindow(oldInstance);
	auto wasVisible = this->window != nullptr && this->window->isVisible();
	this->ensureQWindow();

	// The qml engine will leave the WindowInterface as owner of everything
	// nested in an item, so we have to make sure the interface's children
	// are also reloaded.
	// Reparenting from the interface does not work reliably, so instead
	// we check if the parent is one, as it proxies reloads to here.
	if (auto* w = qobject_cast<WindowInterface*>(this->parent())) {
		for (auto* child: w->children()) {
			if (child == this) continue;
			auto* oldInterfaceParent = oldInstance == nullptr ? nullptr : oldInstance->parent();
			Reloadable::reloadRecursive(child, oldInterfaceParent);
		}
	}

	Reloadable::reloadChildrenRecursive(this, oldInstance);

	this->connectWindow();
	this->completeWindow();

	this->reloadComplete = true;

	emit this->windowConnected();
	this->postCompleteWindow();

	if (wasVisible && this->isVisibleDirect()) {
		emit this->backerVisibilityChanged();
		this->runLints();
	}
}

void ProxyWindowBase::postCompleteWindow() { this->setVisible(this->mVisible); }

ProxiedWindow* ProxyWindowBase::createQQuickWindow() { return new ProxiedWindow(this); }

void ProxyWindowBase::ensureQWindow() {
	auto format = QSurfaceFormat::defaultFormat();

	{
		// match QtQuick's default format, including env var controls
		static const auto useDepth = qEnvironmentVariableIsEmpty("QSG_NO_DEPTH_BUFFER");
		static const auto useStencil = qEnvironmentVariableIsEmpty("QSG_NO_STENCIL_BUFFER");
		static const auto enableDebug = qEnvironmentVariableIsSet("QSG_OPENGL_DEBUG");
		static const auto disableVSync = qEnvironmentVariableIsSet("QSG_NO_VSYNC");

		if (useDepth && format.depthBufferSize() == -1) format.setDepthBufferSize(24);
		else if (!useDepth) format.setDepthBufferSize(0);

		if (useStencil && format.stencilBufferSize() == -1) format.setStencilBufferSize(8);
		else if (!useStencil) format.setStencilBufferSize(0);

		auto opaque = this->qsSurfaceFormat.opaqueModified ? this->qsSurfaceFormat.opaque
		                                                   : this->mColor.alpha() >= 255;

		if (opaque) format.setAlphaBufferSize(0);
		else format.setAlphaBufferSize(8);

		if (enableDebug) format.setOption(QSurfaceFormat::DebugContext);
		if (disableVSync) format.setSwapInterval(0);

		format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
		format.setRedBufferSize(8);
		format.setGreenBufferSize(8);
		format.setBlueBufferSize(8);
	}

	this->mSurfaceFormat = format;

	auto useOldWindow = this->window != nullptr;

	if (useOldWindow) {
		if (this->window->requestedFormat() != format) {
			useOldWindow = false;
		}
	}

	if (useOldWindow) return;
	delete this->window;
	this->window = this->createQQuickWindow();
	this->window->setFormat(format);
}

void ProxyWindowBase::createWindow() {
	this->ensureQWindow();
	this->connectWindow();
	this->completeWindow();
	emit this->windowConnected();
}

void ProxyWindowBase::deleteWindow(bool keepItemOwnership) {
	if (this->window != nullptr) emit this->windowDestroyed();
	if (auto* window = this->disownWindow(keepItemOwnership)) {
		if (auto* generation = EngineGeneration::findObjectGeneration(this)) {
			generation->deregisterIncubationController(window->incubationController());
		}

		window->deleteLater();
	}
}

ProxiedWindow* ProxyWindowBase::disownWindow(bool keepItemOwnership) {
	if (this->window == nullptr) return nullptr;

	QObject::disconnect(this->window, nullptr, this, nullptr);

	if (!keepItemOwnership) {
		this->mContentItem->setParentItem(nullptr);
	}

	auto* window = this->window;
	this->window = nullptr;
	return window;
}

ProxiedWindow* ProxyWindowBase::retrieveWindow(QObject* oldInstance) {
	auto* old = qobject_cast<ProxyWindowBase*>(oldInstance);
	return old == nullptr ? nullptr : old->disownWindow();
}

void ProxyWindowBase::connectWindow() {
	if (auto* generation = EngineGeneration::findObjectGeneration(this)) {
		// All windows have effectively the same incubation controller so it dosen't matter
		// which window it belongs to. We do want to replace the delay one though.
		generation->registerIncubationController(this->window->incubationController());
	}

	this->window->setProxy(this);

	// clang-format off
	QObject::connect(this->window, &QWindow::visibilityChanged, this, &ProxyWindowBase::visibleChanged);
	QObject::connect(this->window, &QWindow::xChanged, this, &ProxyWindowBase::xChanged);
	QObject::connect(this->window, &QWindow::yChanged, this, &ProxyWindowBase::yChanged);
	QObject::connect(this->window, &QWindow::widthChanged, this, &ProxyWindowBase::widthChanged);
	QObject::connect(this->window, &QWindow::heightChanged, this, &ProxyWindowBase::heightChanged);
	QObject::connect(this->window, &QWindow::screenChanged, this, &ProxyWindowBase::screenChanged);
	QObject::connect(this->window, &QQuickWindow::colorChanged, this, &ProxyWindowBase::colorChanged);
	QObject::connect(this->window, &ProxiedWindow::exposed, this, &ProxyWindowBase::runLints);
	// clang-format on
}

void ProxyWindowBase::completeWindow() {
	if (this->mScreen != nullptr && this->window->screen() != this->mScreen) {
		if (this->window->isVisible()) this->window->setVisible(false);
		this->window->setScreen(this->mScreen);
	} else if (this->mScreen == nullptr) {
		this->mScreen = this->window->screen();
		QObject::connect(this->mScreen, &QObject::destroyed, this, &ProxyWindowBase::onScreenDestroyed);
	}

	this->setWidth(this->mWidth);
	this->setHeight(this->mHeight);
	this->setColor(this->mColor);
	this->updateMask();

	// notify initial / post-connection geometry
	emit this->xChanged();
	emit this->yChanged();
	emit this->widthChanged();
	emit this->heightChanged();

	this->mContentItem->setParentItem(this->window->contentItem());
	this->mContentItem->setWidth(this->width());
	this->mContentItem->setHeight(this->height());

	// without this the dangling screen pointer wont be updated to a real screen
	emit this->screenChanged();
}

bool ProxyWindowBase::deleteOnInvisible() const { return false; }

QQuickWindow* ProxyWindowBase::backingWindow() const { return this->window; }
QQuickItem* ProxyWindowBase::contentItem() const { return this->mContentItem; }

bool ProxyWindowBase::isVisible() const {
	if (this->window == nullptr) return this->mVisible;
	else return this->isVisibleDirect();
}

bool ProxyWindowBase::isVisibleDirect() const {
	if (this->window == nullptr) return false;
	else return this->window->isVisible();
}

void ProxyWindowBase::setVisible(bool visible) {
	this->mVisible = visible;
	if (this->reloadComplete) this->setVisibleDirect(visible);
}

void ProxyWindowBase::setVisibleDirect(bool visible) {
	if (this->deleteOnInvisible()) {
		if (visible == this->isVisibleDirect()) return;

		if (visible) {
			this->createWindow();
			this->polishItems();
			this->window->setVisible(true);
			emit this->backerVisibilityChanged();
		} else {
			if (this->window != nullptr) {
				this->window->setVisible(false);
				emit this->backerVisibilityChanged();
				this->deleteWindow();
			}
		}
	} else if (this->window != nullptr) {
		if (visible) this->polishItems();
		this->window->setVisible(visible);
		emit this->backerVisibilityChanged();
	}
}

void ProxyWindowBase::polishItems() {
	// Due to QTBUG-126704, layouts in invisible windows don't update their dimensions.
	// Usually this isn't an issue, but it is when the size of a window is based on the size
	// of its content, and that content is in a layout.
	//
	// This hack manually polishes the item tree right before showing the window so it will
	// always be created with the correct size.
	QQuickWindowPrivate::get(this->window)->polishItems();
}

void ProxyWindowBase::runLints() {
	if (!this->ranLints) {
		qs::debug::lintItemTree(this->mContentItem);
		this->ranLints = true;
	}
}

qint32 ProxyWindowBase::x() const {
	if (this->window == nullptr) return 0;
	else return this->window->x();
}

qint32 ProxyWindowBase::y() const {
	if (this->window == nullptr) return 0;
	else return this->window->y();
}

qint32 ProxyWindowBase::width() const {
	if (this->window == nullptr) return this->mWidth;
	else return this->window->width();
}

void ProxyWindowBase::setWidth(qint32 width) {
	this->mWidth = width;
	if (this->window == nullptr) {
		emit this->widthChanged();
	} else this->window->setWidth(width);
}

qint32 ProxyWindowBase::height() const {
	if (this->window == nullptr) return this->mHeight;
	else return this->window->height();
}

void ProxyWindowBase::setHeight(qint32 height) {
	this->mHeight = height;
	if (this->window == nullptr) {
		emit this->heightChanged();
	} else this->window->setHeight(height);
}

void ProxyWindowBase::setScreen(QuickshellScreenInfo* screen) {
	auto* qscreen = screen == nullptr ? nullptr : screen->screen;
	if (qscreen == this->mScreen) return;

	if (this->mScreen != nullptr) {
		QObject::disconnect(this->mScreen, nullptr, this, nullptr);
	}

	if (this->window == nullptr) {
		emit this->screenChanged();
	} else {
		auto reshow = this->isVisibleDirect();
		if (reshow) this->setVisibleDirect(false);
		if (this->window != nullptr) this->window->setScreen(qscreen);
		if (reshow) this->setVisibleDirect(true);
	}

	if (qscreen) this->mScreen = qscreen;
	else this->mScreen = this->window->screen();

	QObject::connect(this->mScreen, &QObject::destroyed, this, &ProxyWindowBase::onScreenDestroyed);
}

void ProxyWindowBase::onScreenDestroyed() { this->mScreen = nullptr; }

QuickshellScreenInfo* ProxyWindowBase::screen() const {
	QScreen* qscreen = nullptr;

	if (this->window == nullptr) {
		if (this->mScreen != nullptr) qscreen = this->mScreen;
	} else {
		qscreen = this->window->screen();
	}

	return QuickshellTracked::instance()->screenInfo(qscreen);
}

QColor ProxyWindowBase::color() const { return this->mColor; }

void ProxyWindowBase::setColor(QColor color) {
	this->mColor = color;

	if (this->window == nullptr) {
		if (color != this->mColor) emit this->colorChanged();
	} else {
		auto premultiplied = QColor::fromRgbF(
		    color.redF() * color.alphaF(),
		    color.greenF() * color.alphaF(),
		    color.blueF() * color.alphaF(),
		    color.alphaF()
		);

		this->window->setColor(premultiplied);
		// setColor also modifies the alpha buffer size of the surface format
		this->window->setFormat(this->mSurfaceFormat);
	}
}

PendingRegion* ProxyWindowBase::mask() const { return this->mMask; }

void ProxyWindowBase::setMask(PendingRegion* mask) {
	if (mask == this->mMask) return;

	if (this->mMask != nullptr) {
		QObject::disconnect(this->mMask, nullptr, this, nullptr);
	}

	this->mMask = mask;

	if (mask != nullptr) {
		mask->setParent(this);
		QObject::connect(mask, &QObject::destroyed, this, &ProxyWindowBase::onMaskDestroyed);
		QObject::connect(mask, &PendingRegion::changed, this, &ProxyWindowBase::maskChanged);
	}

	emit this->maskChanged();
}

void ProxyWindowBase::setSurfaceFormat(QsSurfaceFormat format) {
	if (format == this->qsSurfaceFormat) return;
	if (this->window != nullptr) {
		qmlWarning(this) << "Cannot set window surface format.";
		return;
	}

	this->qsSurfaceFormat = format;
	emit this->surfaceFormatChanged();
}

void ProxyWindowBase::onMaskChanged() {
	if (this->window != nullptr) this->updateMask();
}

void ProxyWindowBase::onMaskDestroyed() {
	this->mMask = nullptr;
	emit this->maskChanged();
}

void ProxyWindowBase::updateMask() {
	QRegion mask;
	if (this->mMask != nullptr) {
		// if left as the default, dont combine it with the whole window area, leave it as is.
		if (this->mMask->mIntersection == Intersection::Combine) {
			mask = this->mMask->build();
		} else {
			auto windowRegion = QRegion(QRect(0, 0, this->width(), this->height()));
			mask = this->mMask->applyTo(windowRegion);
		}
	}

	this->window->setFlag(Qt::WindowTransparentForInput, this->mMask != nullptr && mask.isEmpty());
	this->window->setMask(mask);
}

QQmlListProperty<QObject> ProxyWindowBase::data() {
	return this->mContentItem->property("data").value<QQmlListProperty<QObject>>();
}

void ProxyWindowBase::onWidthChanged() { this->mContentItem->setWidth(this->width()); }
void ProxyWindowBase::onHeightChanged() { this->mContentItem->setHeight(this->height()); }

ProxyWindowAttached::ProxyWindowAttached(QQuickItem* parent): QsWindowAttached(parent) {
	this->updateWindow();
}

QObject* ProxyWindowAttached::window() const { return this->mWindow; }
QQuickItem* ProxyWindowAttached::contentItem() const { return this->mWindow->contentItem(); }

void ProxyWindowAttached::updateWindow() {
	auto* window = static_cast<QQuickItem*>(this->parent())->window(); // NOLINT

	if (auto* proxy = qobject_cast<ProxiedWindow*>(window)) {
		this->setWindow(proxy->proxy());
	} else {
		this->setWindow(nullptr);
	}
}

void ProxyWindowAttached::setWindow(ProxyWindowBase* window) {
	if (window == this->mWindow) return;
	this->mWindow = window;
	emit this->windowChanged();
}

void ProxiedWindow::exposeEvent(QExposeEvent* event) {
	this->QQuickWindow::exposeEvent(event);
	emit this->exposed();
}
