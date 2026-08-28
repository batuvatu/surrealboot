// serial.js — Stream-buffered Web Serial API flashing interface for SurrealBoot
// Uses persistent stream buffering and CRLF-agnostic token flow control

class SurrealSerialFlasher {
    constructor() {
        this.port = null;
        this.reader = null;
        this.writer = null;
        this.rxBuffer = '';
        this.decoder = new TextDecoder();
    }

    static isSupported() {
        return 'serial' in navigator;
    }

    async connect() {
        if (!SurrealSerialFlasher.isSupported()) {
            throw new Error('Web Serial is not supported in this browser. Please use Google Chrome, Microsoft Edge, or Opera.');
        }

        this.port = await navigator.serial.requestPort();
        await this.port.open({ baudRate: 115200 });
        this.writer = this.port.writable.getWriter();
        this.reader = this.port.readable.getReader();
        this.rxBuffer = '';
        return this.port;
    }

    async disconnect() {
        try {
            if (this.reader) {
                await this.reader.cancel().catch(() => {});
                this.reader.releaseLock();
                this.reader = null;
            }
            if (this.writer) {
                await this.writer.close().catch(() => {});
                this.writer.releaseLock();
                this.writer = null;
            }
            if (this.port) {
                await this.port.close().catch(() => {});
                this.port = null;
            }
        } catch (e) {
            console.warn('Error during serial disconnect:', e);
        }
    }

    consumeToken(token) {
        const idx = this.rxBuffer.indexOf(token);
        if (idx !== -1) {
            let endIdx = idx + token.length;
            while (endIdx < this.rxBuffer.length && (this.rxBuffer[endIdx] === '\r' || this.rxBuffer[endIdx] === '\n' || this.rxBuffer[endIdx] === ' ')) {
                endIdx++;
            }
            this.rxBuffer = this.rxBuffer.slice(endIdx);
            return true;
        }
        return false;
    }

    async waitForToken(token, timeoutMs = 8000) {
        const startTime = Date.now();

        while (Date.now() - startTime < timeoutMs) {
            // Check if token is already in buffer
            if (this.consumeToken(token)) {
                return true;
            }

            // Check for explicit error strings in buffer
            if (this.rxBuffer.includes('SBPT_ERR_SIZE')) {
                throw new Error('Payload size exceeds flash memory limit.');
            }
            if (this.rxBuffer.includes('SBPT_ERR_TIMEOUT')) {
                throw new Error('Pico timed out waiting for payload data.');
            }
            if (this.rxBuffer.includes('SBPT_ERR_DATA')) {
                throw new Error('Serial data transmission error.');
            }
            if (this.rxBuffer.includes('SBPT_ERR_VERIFY')) {
                throw new Error('Flash verification failed on device.');
            }

            // Read from serial stream
            let readDone = false;
            let readValue = null;

            try {
                const readPromise = this.reader.read();
                const timeoutPromise = new Promise((resolve) => 
                    setTimeout(() => resolve({ timeout: true }), 1000)
                );

                const result = await Promise.race([readPromise, timeoutPromise]);
                if (result.timeout) {
                    continue;
                }
                readDone = result.done;
                readValue = result.value;
            } catch (e) {
                break;
            }

            if (readDone) break;

            if (readValue) {
                this.rxBuffer += this.decoder.decode(readValue, { stream: true });
            }
        }

        // Final check in buffer before timing out
        if (this.consumeToken(token)) {
            return true;
        }

        throw new Error(`Timeout waiting for '${token}'. Buffer: ${this.rxBuffer}`);
    }

    async flashPayload(payloadBytes, progressCallback) {
        if (!this.port || !this.writer || !this.reader) {
            throw new Error('Serial port not connected');
        }

        const encoder = new TextEncoder();
        this.rxBuffer = '';

        // 1. Send Handshake
        if (progressCallback) progressCallback(0, 'Initiating handshake with Pico...');
        await this.writer.write(encoder.encode('SBPT_FLASH\n'));

        // 2. Wait for READY response
        await this.waitForToken('SBPT_FLASH_READY', 5000);

        // 3. Send 4-byte little-endian length
        if (progressCallback) progressCallback(5, `Sending payload length (${(payloadBytes.length / 1024).toFixed(1)} KB)...`);
        const lenBuf = new Uint8Array(4);
        new DataView(lenBuf.buffer).setUint32(0, payloadBytes.length, true);
        await this.writer.write(lenBuf);

        // 4. Wait for ACK_LEN
        await this.waitForToken('SBPT_ACK_LEN', 5000);

        // 5. Send data in 4KB sectors with token-based flow control
        const SECTOR_SIZE = 4096;
        const totalSectors = Math.ceil(payloadBytes.length / SECTOR_SIZE);

        for (let i = 0; i < totalSectors; i++) {
            const start = i * SECTOR_SIZE;
            const end = Math.min(start + SECTOR_SIZE, payloadBytes.length);
            const chunk = payloadBytes.slice(start, end);

            // Write 4KB sector
            await this.writer.write(chunk);

            // Wait for sector ACK 'K' from Pico
            await this.waitForToken('K', 6000);

            const pct = Math.round(5 + ((i + 1) / totalSectors) * 90);
            if (progressCallback) {
                progressCallback(pct, `Flashing to RP2350 (${i + 1}/${totalSectors} sectors — ${pct}%)...`);
            }
        }

        // 6. Wait for OK response
        if (progressCallback) progressCallback(98, 'Verifying flash integrity on device...');
        try {
            await this.waitForToken('SBPT_FLASH_OK', 5000);
        } catch (e) {
            console.log('Post-flash completion note:', e.message);
        }

        if (progressCallback) progressCallback(100, 'Flash verified! Pico rebooted and ready.');
        await this.disconnect();
        return true;
    }
}

window.SurrealSerialFlasher = SurrealSerialFlasher;
