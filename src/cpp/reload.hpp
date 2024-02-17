#pragma once

#include <qobject.h>
#include <qqmlcomponent.h>
#include <qqmlintegration.h>
#include <qqmllist.h>
#include <qqmlparserstatus.h>
#include <qtmetamacros.h>

///! The base class of all types that can be reloaded.
/// Reloadables will attempt to take specific state from previous config revisions if possible.
/// Some examples are `ProxyShellWindow` and `ProxyFloatingWindow` which will attempt to find the
/// windows assigned to them in the previous configuration.
class Reloadable: public QObject, public QQmlParserStatus {
	Q_OBJECT;
	Q_INTERFACES(QQmlParserStatus);
	/// An additional identifier that can be used to try to match a reloadable object to its
	/// previous state.
	///
	/// Simply keeping a stable identifier across config versions (saves) is
	/// enough to help the reloader figure out which object in the old revision corrosponds to
	/// this object in the current revision, and facilitate smoother reloading.
	///
	/// Note that identifiers are scoped, and will try to do the right thing in context.
	/// For example if you have a `Variants` wrapping an object with an identified element inside,
	/// a scope is created at the variant level.
	///
	/// ```qml
	/// Variants {
	///   // multiple variants of the same object tree
	///   variants: [ { foo: 1 }, { foo: 2 } ]
	///
	///   // any non `Reloadable` object
	///   QtObject {
	///     ProxyFloatingWindow {
	///       // this ProxyFloatingWindow will now be matched to the same one in the previous
	///       // widget tree for its variant. "myFloatingWindow" refers to both the variant in
	///       // `foo: 1` and `foo: 2` for each tree.
	///       reloadableId: "myFloatingWindow"
	///
	///       // ...
	///     }
	///   }
	/// }
	/// ```
	Q_PROPERTY(QString reloadableId MEMBER mReloadableId);
	QML_ELEMENT;
	QML_UNCREATABLE(
	    "Reloadable is the base class of reloadable types and cannot be created on its own."
	);

public:
	explicit Reloadable(QObject* parent = nullptr): QObject(parent) {}

	// Called unconditionally in the reload phase, with nullptr if no source could be determined.
	// If non null the old instance may or may not be of the same type, and should be checked
	// by `onReload`.
	virtual void onReload(QObject* oldInstance) = 0;

	// TODO: onReload runs after initialization for reloadable objects created late
	void classBegin() override {}
	void componentComplete() override {}

	// Reload objects in the parent->child graph recursively.
	static void reloadRecursive(QObject* newObj, QObject* oldRoot);
	// Same as above but does not reload the passed object, only its children.
	static void reloadChildrenRecursive(QObject* newRoot, QObject* oldRoot);

	QString mReloadableId;

private:
	static QObject* getChildByReloadId(QObject* parent, const QString& reloadId);
};

///! Basic type that propagates reloads to child items in order.
/// Convenience type equivalent to setting `reloadableId` on properties in a
/// QtObject instance.
///
/// Note that this does not work for visible `Item`s (all widgets).
///
/// ```qml
/// ShellRoot {
///   Variants {
///     variants: ...
///
///     ReloadPropagator {
///       // everything in here behaves the same as if it was defined
///       // directly in `Variants` reload-wise.
///     }
///   }
/// }
class ReloadPropagator: public Reloadable {
	Q_OBJECT;
	Q_PROPERTY(QQmlListProperty<QObject> children READ data);
	Q_CLASSINFO("DefaultProperty", "children");
	QML_ELEMENT;

public:
	explicit ReloadPropagator(QObject* parent = nullptr): Reloadable(parent) {}

	void onReload(QObject* oldInstance) override;

	QQmlListProperty<QObject> data();

private:
	static void appendComponent(QQmlListProperty<QObject>* list, QObject* obj);

	QList<QObject*> mChildren;
};
