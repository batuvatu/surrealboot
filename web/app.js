// app.js — Main application logic for SurrealBoot Web Flasher

document.addEventListener('DOMContentLoaded', () => {
    // State
    let selectedBoard = 'waveshare_rp2350_usb_a';
    let loadedFile = null;
    let compressedPayload = null;
    const serialFlasher = new SurrealSerialFlasher();

    // DOM Elements
    const boardOptions = document.querySelectorAll('.board-list li label');
    const downloadBaseBtn = document.getElementById('downloadBaseBtn');
    const baseSizeHint = document.getElementById('baseSizeHint');
    const dropZone = document.getElementById('dropZone');
    const fileInput = document.getElementById('fileInput');
    const fileInfoBox = document.getElementById('fileInfoBox');
    const loadedFileName = document.getElementById('loadedFileName');
    const clearFileBtn = document.getElementById('clearFileBtn');
    const statRawSize = document.getElementById('statRawSize');
    const statCompSize = document.getElementById('statCompSize');
    const statRatio = document.getElementById('statRatio');
    const flashSerialBtn = document.getElementById('flashSerialBtn');
    const flashProgressBox = document.getElementById('flashProgressBox');
    const meterFill = document.getElementById('meterFill');
    const progressStateText = document.getElementById('progressStateText');
    const progressPercentText = document.getElementById('progressPercentText');
    const consoleOutput = document.getElementById('consoleOutput');
    const serialStatus = document.getElementById('serialStatus');

    // Check Web Serial Support
    if (!SurrealSerialFlasher.isSupported()) {
        const warn = document.createElement('p');
        warn.style.cssText = 'color:#e05555;font-size:12px;margin-top:8px';
        warn.textContent = 'Web Serial not supported — use Chrome or Edge.';
        flashSerialBtn.insertAdjacentElement('afterend', warn);
    }

    // Helper: Console Log
    function log(msg) {
        const time = new Date().toLocaleTimeString();
        consoleOutput.textContent += `\n[${time}] ${msg}`;
        consoleOutput.scrollTop = consoleOutput.scrollHeight;
    }

    function formatBytes(bytes) {
        if (bytes < 1024) return bytes + ' B';
        if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
        return (bytes / (1024 * 1024)).toFixed(2) + ' MB';
    }

    // Board Selection
    boardOptions.forEach(opt => {
        opt.addEventListener('click', () => {
            boardOptions.forEach(o => o.classList.remove('active'));
            opt.classList.add('active');
            const radio = opt.querySelector('input[type="radio"]');
            if (radio) {
                radio.checked = true;
                selectedBoard = radio.value;
            }
            updateBaseDownloadInfo();
        });
    });

    function updateBaseDownloadInfo() {
        baseSizeHint.textContent = 'v1.0.0 via GitHub Releases';
    }
    updateBaseDownloadInfo();

    // Download Base Firmware (.UF2) from GitHub Releases
    downloadBaseBtn.addEventListener('click', () => {
        const releaseUrl = `https://github.com/batuvatu/surrealboot/releases/latest/download/usbliter8-bootsurreal-${selectedBoard}.uf2`;
        
        // Trigger download
        const a = document.createElement('a');
        a.href = releaseUrl;
        a.download = `usbliter8-bootsurreal-${selectedBoard}.uf2`;
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
    });

    // File Drag & Drop
    dropZone.addEventListener('click', () => fileInput.click());

    dropZone.addEventListener('dragover', (e) => {
        e.preventDefault();
        dropZone.classList.add('dragover');
    });

    dropZone.addEventListener('dragleave', () => {
        dropZone.classList.remove('dragover');
    });

    dropZone.addEventListener('drop', (e) => {
        e.preventDefault();
        dropZone.classList.remove('dragover');
        if (e.dataTransfer.files.length > 0) {
            handleFile(e.dataTransfer.files[0]);
        }
    });

    fileInput.addEventListener('change', () => {
        if (fileInput.files.length > 0) {
            handleFile(fileInput.files[0]);
        }
    });

    // Process Uploaded Boot File
    async function handleFile(file) {
        loadedFile = file;
        loadedFileName.textContent = file.name;
        statRawSize.textContent = formatBytes(file.size);

        dropZone.style.display = 'none';
        fileInfoBox.style.display = 'block';
        flashSerialBtn.disabled = true;

        flashProgressBox.style.display = 'flex';
        meterFill.style.width = '20%';
        progressStateText.textContent = 'Compressing with LZ4...';
        progressPercentText.textContent = '20%';
        consoleOutput.textContent = `> Loaded file: ${file.name} (${formatBytes(file.size)})`;

        try {
            const arrayBuffer = await file.arrayBuffer();
            const inputBytes = new Uint8Array(arrayBuffer);

            log('Compressing 64KB blocks using in-browser LZ4...');
            const compressFn = window.compressBootFile || window.compressPayload;
            if (typeof compressFn !== 'function') {
                throw new Error("LZ4 compressor module not loaded");
            }
            const compressed = compressFn(inputBytes);

            compressedPayload = compressed;
            statCompSize.textContent = formatBytes(compressed.data.length);
            const ratio = ((compressed.data.length / inputBytes.length) * 100).toFixed(1);
            statRatio.textContent = `${ratio}% (${compressed.chunkCount} chunks)`;

            log(`Compressed: ${inputBytes.length} -> ${compressed.data.length} bytes (${ratio}%)`);
            log(`Flash destination: offset 0x10020000 (Header: "SBPT")`);

            meterFill.style.width = '100%';
            progressStateText.textContent = 'Ready to flash over Serial.';
            progressPercentText.textContent = '100%';

            flashSerialBtn.disabled = false;
        } catch (err) {
            console.error(err);
            log(`Compression error: ${err.message}`);
            alert(`Error processing boot file: ${err.message}`);
        }
    }

    // Clear / Remove File
    clearFileBtn.addEventListener('click', () => {
        loadedFile = null;
        compressedPayload = null;
        fileInput.value = '';
        dropZone.style.display = 'block';
        fileInfoBox.style.display = 'none';
        flashSerialBtn.disabled = true;
        flashProgressBox.style.display = 'none';
        meterFill.style.width = '0%';
    });

    // Flash via Web Serial
    flashSerialBtn.addEventListener('click', async () => {
        if (!compressedPayload) {
            alert('Please select an iBSS.boot file first.');
            return;
        }

        if (!SurrealSerialFlasher.isSupported()) {
            alert('Web Serial is not supported in this browser. Please use Google Chrome, Microsoft Edge, or Opera.');
            return;
        }

        flashSerialBtn.disabled = true;
        flashProgressBox.style.display = 'flex';
        meterFill.style.width = '0%';
        progressStateText.textContent = 'Connecting to Pico...';
        progressPercentText.textContent = '0%';

        log('Requesting Web Serial port...');

        try {
            await serialFlasher.connect();
            log('Serial connection established (115200 baud).');

            await serialFlasher.flashPayload(compressedPayload.data, (pct, status) => {
                meterFill.style.width = `${pct}%`;
                progressPercentText.textContent = `${pct}%`;
                progressStateText.textContent = status;
                log(status);
            });

            log('Flash verification passed. Microcontroller rebooted into hardware booter.');
            alert('Flash Successful! Your RP2350 is now loaded and ready to tether-boot on the go.');

        } catch (err) {
            console.error('Serial flashing error:', err);
            log(`ERROR: ${err.message}`);
            progressStateText.textContent = `Error: ${err.message}`;
            alert(`Serial Flash Error: ${err.message}\n\nPlease ensure your board is plugged into USB and running the base firmware.`);
            await serialFlasher.disconnect();
        } finally {
            flashSerialBtn.disabled = false;
        }
    });
});
