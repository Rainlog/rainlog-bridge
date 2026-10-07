// Manual app-image upload. The firmware repeats all identity checks and verifies
// the complete image before selecting the inactive OTA slot for boot.
export function createManualUpdate() {
  const fileInput = document.getElementById('firmwareFile') as HTMLInputElement;
  const install = document.getElementById(
    'firmwareInstall',
  ) as HTMLButtonElement;
  const cancel = document.getElementById('firmwareCancel') as HTMLButtonElement;
  const info = document.getElementById('firmwareFileInfo')!;
  const progress = document.getElementById('firmwareUploadStatus')!;
  let board = '',
    capacity = 0,
    busy = false,
    serverBusy = false;
  let selected: File | null = null;
  let request: XMLHttpRequest | null = null;

  function controls(): void {
    install.disabled = !selected || busy || serverBusy;
    fileInput.disabled = busy;
  }
  async function inspect(): Promise<void> {
    selected = null;
    controls();
    const file = fileInput.files?.[0];
    if (!file) {
      info.textContent = '';
      return;
    }
    if (!board || !capacity) {
      info.textContent =
        'Wait for the bridge firmware status, then select the file again.';
      return;
    }
    if (file.size < 336 || file.size > capacity) {
      info.textContent = 'This file does not fit the firmware update slot.';
      return;
    }
    try {
      const bytes = new Uint8Array(await file.slice(0, 336).arrayBuffer());
      if (file !== fileInput.files?.[0]) return;
      const text = (offset: number, size: number): string => {
        const value = bytes.slice(offset, offset + size);
        const end = value.indexOf(0);
        if (end < 0) throw new Error('Invalid firmware description.');
        return new TextDecoder().decode(value.slice(0, end));
      };
      if (
        bytes[0] !== 0xe9 ||
        new DataView(bytes.buffer).getUint32(32, true) !== 0xabcd5432 ||
        text(80, 32) !== 'rainlog-wireless-bridge'
      )
        throw new Error(
          'Select a Rainlog Bridge app .bin, not a merged or bootloader image.',
        );
      if (new TextDecoder().decode(bytes.slice(288, 296)) !== 'RLOGOTA1')
        throw new Error(
          'This image lacks board identity. Rebuild it with the current project.',
        );
      if (text(296, 40) !== board)
        throw new Error('This firmware is for a different board.');
      if (bytes[23] !== 1)
        throw new Error('This firmware lacks its integrity hash.');
      const version = text(48, 32);
      if (!/^[A-Za-z0-9._+-]+$/.test(version))
        throw new Error('Invalid firmware version.');
      info.textContent = `Version ${version} · ${board} · ${file.size.toLocaleString()} bytes. Same-version installs and downgrades are allowed.`;
      selected = file;
    } catch (error) {
      info.textContent =
        error instanceof Error
          ? error.message
          : 'Unable to read the firmware file.';
    }
    controls();
  }
  fileInput.addEventListener('change', () => void inspect());
  cancel.addEventListener('click', () => request?.abort());
  install.addEventListener('click', () => {
    if (!selected || busy || serverBusy) return;
    busy = true;
    controls();
    const xhr = new XMLHttpRequest();
    request = xhr;
    xhr.open('POST', '/ota/upload');
    xhr.setRequestHeader('Content-Type', 'application/octet-stream');
    xhr.setRequestHeader('X-Rainlog-OTA', '1');
    cancel.hidden = false;
    progress.textContent = 'Uploading firmware…';
    (document.getElementById('otaCheckBtn') as HTMLButtonElement).disabled =
      true;
    (document.getElementById('otaApplyBtn') as HTMLButtonElement).disabled =
      true;
    xhr.upload.onprogress = (event) => {
      if (event.lengthComputable) {
        const percent = Math.round((event.loaded * 100) / event.total);
        progress.textContent =
          percent === 100
            ? 'Upload complete. Validating firmware…'
            : `Uploading firmware… ${percent}%`;
        cancel.hidden = percent === 100;
      }
    };
    const done = (message: string, rebooting = false): void => {
      progress.textContent = message;
      cancel.hidden = true;
      request = null;
      busy = rebooting;
      controls();
      if (!rebooting)
        (document.getElementById('otaCheckBtn') as HTMLButtonElement).disabled =
          false;
    };
    xhr.onload = () => {
      let result: { ok?: boolean; error?: string } = {};
      try {
        result = JSON.parse(xhr.responseText) as typeof result;
      } catch {
        /* HTTP error without JSON. */
      }
      if (xhr.status === 200 && result.ok)
        done(
          'Firmware verified. Rebooting; reconnect to the bridge shortly.',
          true,
        );
      else done(result.error ?? 'Firmware upload was rejected.');
    };
    xhr.onerror = () =>
      done(
        'Connection lost. Reconnect and check the running firmware before retrying.',
      );
    xhr.onabort = () =>
      done('Upload canceled. The incomplete image will not be installed.');
    xhr.send(selected);
  });
  return {
    setStatus(status: {
      board?: string;
      max_image_size?: number;
      phase: string;
    }): void {
      board = status.board ?? '';
      capacity = status.max_image_size ?? 0;
      serverBusy = status.phase === 'checking' || status.phase === 'updating';
      controls();
    },
  };
}
