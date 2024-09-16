import WebUSBSerial from '@leaphy-robotics/webusb-ftdi';

let port;
let opts;
let writer;
let reader;

let onData
let writeBuffer
let readBuffer
let writeAddressBuf
let readAddressBuf

const readPromise = (customReader) => new Promise(async () => {
    while (true) {
        const { value, done } = await customReader.read()
        if (done) break

        let address = readAddressBuf[0];
        let read = 0;

        while (read !== value.length) {
            const initialLength = Math.min(value.length, readBuffer.length - address);
            readBuffer.set(value.slice(read, read + initialLength), address);
            read += initialLength;
            address += initialLength;

            if (address === readBuffer.length) {
                address = 3;
            }
        }
        readAddressBuf[0] = address;

        onData && onData()
    }
})

function readFromBuffer(currentAddress, targetAddress, buffer) {
    if (currentAddress < targetAddress) {
        return buffer.slice(currentAddress, targetAddress)
    }

    const array = new Uint8Array(buffer.length - currentAddress + targetAddress - 3)
    array.set(buffer.slice(currentAddress))
    array.set(buffer.slice(3, targetAddress), buffer.length - currentAddress)

    return array
}

addEventListener('message', async msg => {
    try {
        const data = msg.data

        switch (data.type) {
            case 'clear-read-buffer': {
                const timeoutPromise = new Promise(resolve => setTimeout(resolve, data.timeout))
                const onDone = new Promise(resolve => onData = resolve)

                await Promise.race([timeoutPromise, onDone])
                readAddressBuf[0] = 3

                postMessage({type: 'clear-read-buffer'})
                break
            }
            case 'init': {
                if (navigator.serial) {
                    port = (await navigator.serial.getPorts())[data.port]
                } else {
                    const device = (await navigator.usb.getDevices())[data.port]
                    port = new WebUSBSerial(device)
                }

                readBuffer = new Uint8Array(data.readBuffer)
                writeBuffer = new Uint8Array(data.writeBuffer)
                readAddressBuf = new Uint16Array(data.readBuffer)
                writeAddressBuf = new Uint16Array(data.writeBuffer)

                await port.open(data.options)
                opts = data.options
                writer = port.writable.getWriter()
                reader = port.readable.getReader()

                let address = 3
                setInterval(async () => {
                    if (writeAddressBuf[0] === address) return

                    const target = writeAddressBuf[0]
                    const data = readFromBuffer(address, target, writeBuffer)
                    await writer.write(data)

                    address = target
                }, 0)

                readPromise(reader).then()
                postMessage({type: 'ready'})
                break
            }
            case 'close': {
                writer.releaseLock()
                reader.cancel()
                reader.releaseLock()
                await port.close()
                postMessage({type: 'closed'})
                break
            }
            default: {
                console.error('Unknown message type', data.type)
                break
            }
        }
    } catch (e) {
        console.error(e)
        postMessage({type: 'error', error: e})
    }

})
