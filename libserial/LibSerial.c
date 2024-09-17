#include "LibSerial.h"
#include <emscripten.h>

unsigned char *readBuffer;
size_t readBufferLen;


void dataCallback(const unsigned char* array, int length) {
    for (size_t i = 0; i < length; ++i) {
        readBuffer[readBufferLen++] = array[i];
    }
}

void errorCallback() {
    exit(1);
}



EM_JS(void, write_data, (unsigned char* buf, int len), {
    let writeAddress = window.writeAddressBuf[0];
    let written = 0;

    while (written !== len) {
        const initialLength = Math.min(len - written, window.writeBuffer.length - writeAddress);
        const data = new Uint8Array(Module.HEAPU8.buffer.slice(buf + written, buf + written + initialLength));
        window.writeBuffer.set(data, writeAddress);
        window.avrdudeLog = [...window.avrdudeLog, "Sending: " + Array.from(data).join(",")];

        written += initialLength;
        writeAddress += initialLength;

        if (writeAddress === window.writeBuffer.length) {
            writeAddress = 3;
        };
    };

    window.writeAddressBuf[0] = writeAddress;
});


EM_ASYNC_JS(void, clear_read_buffer, (int timeoutMs), {
    window.avrdudeLog = [...window.avrdudeLog, "Clearing read buffer"];
    window.avrDudeWorker.postMessage({ type: 'clear-read-buffer', timeout: timeoutMs });
    await new Promise(resolve => {
        window.avrDudeWorker.onmessage = (event) => {
            // check if the response type is an error
            if (event.data.type === "error") {
                window.funcs._errorCallback();
            }
            resolve();
        };
    });
    window.readAddress = 3;
    window.avrdudeLog = [...window.avrdudeLog, "Read buffer cleared"];
});

EM_ASYNC_JS(void, read_data, (int timeoutMs, int length), {
    const result = new Uint8Array(length);
    let read = 0;
    let end = Date.now() + timeoutMs;

    while (read !== length && end > Date.now()) {
        if (window.readAddress === window.readAddressBuf[0]) continue;

        let targetAddress = window.readAddressBuf[0];
        if (targetAddress < window.readAddress) {
            targetAddress = window.readBuffer.length;
        };
        targetAddress = Math.min(targetAddress, window.readAddress + length - read);

        result.set(window.readBuffer.slice(window.readAddress, targetAddress), read);
        read += targetAddress - window.readAddress;

        window.readAddress = targetAddress;
        if (window.readAddress === window.readBuffer.length) {
            window.readAddress = 3;
        }
    }

    if (read < length) {
        window["avrdudeLog"] = [...window["avrdudeLog"], "Timeout"];
        return;
    }

    const ptr = window.funcs._malloc(read * Uint8Array.BYTES_PER_ELEMENT);
    const data = result.slice(0, read);
    window.funcs.HEAPU8.set(result.slice(0, read), ptr);
    window["avrdudeLog"] = [...window["avrdudeLog"], "Received: " + Array.from(data).join(",")];

    window.funcs._dataCallback(ptr, read);
});

// clang-format off
// get serial options as an EM_VAL
EM_ASYNC_JS(void, open_serial_port, (int baudRateInt), {
    const serialOpts = {
        baudRate: baudRateInt,
        bufferSize: 1024*2
    };
    let port;
    if (!window.activePort) {
        port = await navigator.serial.requestPort();
        await port.open(serialOpts);
    } else {
        port = window.activePort;
        // check if open otherwise open port
        if (!port.readable || !port.writable) {
            await port.open(serialOpts);
        }
    }
    // reset the arduino by setting the DTR signal at baud rate 1200
    await port.close();
    await port.open({baudRate: 1200});
    await port.setSignals({dataTerminalReady: false});
    await new Promise(resolve => setTimeout(resolve, 100));
    await port.setSignals({dataTerminalReady: true});
    await port.close();

    const worker = new Worker('avrdude-worker.js', { type: 'module' });

    // check which port in navigator.serial.getPorts matches the activePort
    let portNumber = 0;
    if (navigator.serial) {
       const ports = await navigator.serial.getPorts();
       for (let i = 0; i < ports.length; i++) {
          const port = ports[i];
          if (port === window.activePort) {
             portNumber = i;
             break;
          }
       }
    }

    const writeBuffer = new SharedArrayBuffer(4096);
    const readBuffer = new SharedArrayBuffer(4096);
    worker.postMessage({
        type: 'init',
        options: serialOpts,
        port: portNumber,
        writeBuffer, readBuffer,
    });

    await new Promise(resolve => {
        worker.onmessage = (event) => {
            if (event.data.type === "error") {
                window.funcs._errorCallback();
            }
            resolve();
        };
    });

    // Initialize read and write buffers with address
    window.writeBuffer = new Uint8Array(writeBuffer);
    window.readBuffer = new Uint8Array(readBuffer);
    window.writeAddressBuf = new Uint16Array(writeBuffer);
    window.readAddressBuf = new Uint16Array(readBuffer);
    window.writeAddressBuf[0] = 3;
    window.readAddressBuf[0] = 3;
    window.readAddress = 3;

    // open the port with the correct baud rate
    window.avrDudeWorker = worker;
    window.activePort = port;
    window["avrdudeLog"] = [];
});

EM_ASYNC_JS(void, close_serial_port, (), {
    window.avrDudeWorker.postMessage({ type: 'close' });
    await new Promise(resolve => {
        window.avrDudeWorker.onmessage = (event) => {
            // check if the response type is an error
            if (event.data.type === "error") {
                window.funcs._errorCallback();
            }
            resolve();
        };
    });
    window.activePort = null;
    window.avrDudeWorker.terminate();
});

EM_ASYNC_JS(bool, is_serial_port_open, (), {
    const port = window.activePort;
    return port.readable && port.writable;
});

EM_ASYNC_JS(void, set_dts_rts, (bool is_on), {
    await window.activePort.setSignals({dataTerminalReady: is_on, requestToSend: is_on});
});

int serialPortOpen(int baudRate) {
    open_serial_port(baudRate);
    return 0;
}

void serialPortClose() {
    close_serial_port();
}

void setDtrRts(bool is_on) {
    set_dts_rts(is_on);
}

void serialPortDrain(int timeout) {
    readBufferLen = 0;
    clear_read_buffer(timeout);
}

void serialPortWrite(const unsigned char *buf, size_t len) {
    write_data((unsigned char*)buf, (int)len);
}

int serialPortRecv(unsigned char *buf, size_t len, int timeoutMs) {
    read_data(timeoutMs, len);
    size_t processed = 0;
    if (readBufferLen != 0) {
        if (readBufferLen >= len) {
            for (size_t i = 0; i < len; ++i) {
                buf[i] = readBuffer[i];
                processed++;
            }
            readBufferLen -= len;

        } else {
            for (size_t i = 0; i < readBufferLen; ++i) {
                buf[i] = readBuffer[i];
                processed++;
            }
            readBufferLen = 0;
        }
    }
    if (processed == 0) {
        return -1;
    }
    return 0;
}
