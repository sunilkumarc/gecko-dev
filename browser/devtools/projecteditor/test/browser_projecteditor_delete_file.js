/* vim: set ts=2 et sw=2 tw=80: */
/* Any copyright is dedicated to the Public Domain.
 http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

// Test tree selection functionality

let test = asyncTest(function*() {
  let projecteditor = yield addProjectEditorTabForTempDirectory();
  ok(true, "ProjectEditor has loaded");

  let root = [...projecteditor.project.allStores()][0].root;
  is(root.path, TEMP_PATH, "The root store is set to the correct temp path.");
  for (let child of root.children) {
    yield deleteWithContextMenu(projecteditor.projectTree.getViewContainer(child));
  }

  function onPopupShow(contextMenu) {
    let defer = promise.defer();
    contextMenu.addEventListener("popupshown", function onpopupshown() {
      contextMenu.removeEventListener("popupshown", onpopupshown);
      defer.resolve();
    });
    return defer.promise;
  }

  function onPopupHide(contextMenu) {
    let defer = promise.defer();
    contextMenu.addEventListener("popuphidden", function popuphidden() {
      contextMenu.removeEventListener("popuphidden", popuphidden);
      defer.resolve();
    });
    return defer.promise;
  }

  function openContextMenuOn(node) {
    EventUtils.synthesizeMouseAtCenter(
      node,
      {button: 2, type: "contextmenu"},
      node.ownerDocument.defaultView
    );
  }

  function deleteWithContextMenu(container) {
    let defer = promise.defer();

    let resource = container.resource;
    let popup = projecteditor.document.getElementById("directory-menu-popup");
    info ("Going to attempt deletion for: " + resource.path)

    onPopupShow(popup).then(function () {
      let deleteCommand = popup.querySelector("[command=cmd-delete]");
      ok (deleteCommand, "Delete command exists in popup");
      is (deleteCommand.getAttribute("hidden"), "", "Delete command is visible");
      is (deleteCommand.getAttribute("disabled"), "", "Delete command is enabled");

      onPopupHide(popup).then(() => {
        ok (true, "Popup has been hidden, waiting for project refresh");
        projecteditor.project.refresh().then(() => {
          OS.File.stat(resource.path).then(() => {
            ok (false, "The file was not deleted");
            defer.resolve();
          }, (ex) => {
            ok (ex instanceof OS.File.Error && ex.becauseNoSuchFile, "OS.File.stat promise was rejected because the file is gone");
            defer.resolve();
          });
        });
      });

      deleteCommand.click();
      popup.hidePopup();
    });

    openContextMenuOn(container.label);

    return defer.promise;
  }

});