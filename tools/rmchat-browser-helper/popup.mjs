import { EXPORT_FILENAME, readSessionForExport, serializeCredential, SessionExportError } from "./session.mjs";

const button = document.querySelector("#export");
const status = document.querySelector("#status");
let previousUrl;

function releaseDownload() {
  if (previousUrl) URL.revokeObjectURL(previousUrl);
  previousUrl = undefined;
}

button.addEventListener("click", async () => {
  button.disabled = true;
  status.textContent = "Préparation du fichier local…";
  status.dataset.error = "false";
  releaseDownload();
  try {
    const credential = await readSessionForExport(chrome.cookies);
    const blob = new Blob([serializeCredential(credential)], { type: "application/json;charset=utf-8" });
    previousUrl = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = previousUrl;
    link.download = EXPORT_FILENAME;
    link.hidden = true;
    document.body.append(link);
    link.click();
    link.remove();
    status.textContent = "Téléchargement demandé : rmchat-session.json. Importez ce fichier dans RMChat.";
  } catch (error) {
    releaseDownload();
    status.dataset.error = "true";
    status.textContent = error instanceof SessionExportError ? error.message
      : "L’export n’a pas abouti. Réessayez depuis cette fenêtre.";
  } finally {
    button.disabled = false;
  }
});

window.addEventListener("pagehide", releaseDownload);
