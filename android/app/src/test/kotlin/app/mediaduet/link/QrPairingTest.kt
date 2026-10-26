package app.mediaduet.link

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test

class QrPairingTest {
    private val id = "0123456789abcdef0123456789abcdef"
    private val token = "QEFCQ0RFRkdISUpLTE1OTw" // bytes 0x40..0x4f (crypto-v1.json)

    @Test
    fun parsesThePcCode() {
        val c = QrPairing.parse("mediaduet://pair?v=1&id=$id&t=$token&n=My%20PC%20%E5%B7%A5%E4%BD%9C&p=windows")
        assertNotNull(c)
        assertEquals(id, c!!.peerId)
        assertEquals("My PC 工作", c.peerName)
        assertEquals("windows", c.platform)
        assertArrayEquals(ByteArray(16) { (0x40 + it).toByte() }, c.token)
    }

    @Test
    fun ignoresUnknownParametersAndOrder() {
        val c = QrPairing.parse("mediaduet://pair?x=9&t=$token&p=windows&id=$id&v=1&future=yes")
        assertNotNull(c)
        assertEquals("", c!!.peerName)
    }

    @Test
    fun rejectsMalformedCodes() {
        val ok = "mediaduet://pair?v=1&id=$id&t=$token"
        assertNotNull(QrPairing.parse(ok))
        assertNull(QrPairing.parse(ok.replace("v=1", "v=2")))
        assertNull(QrPairing.parse("mediaduet://pair?id=$id&t=$token")) // no version
        assertNull(QrPairing.parse(ok.replace(id, id.uppercase())))
        assertNull(QrPairing.parse(ok.replace(id, id.dropLast(1))))
        assertNull(QrPairing.parse(ok.replace(token, token.dropLast(1)))) // 15.x bytes
        assertNull(QrPairing.parse(ok.replace(token, "$token" + "AA"))) // 17+ bytes
        assertNull(QrPairing.parse(ok.replace(token, "QEFCQ0RFRkdISUpLTE1OTw==")))  // padded
        assertNull(QrPairing.parse(ok.replace(token, "QEFCQ0RFRkdISUpLTE1O+w"))) // not base64url
        assertNull(QrPairing.parse("$ok&v=1")) // duplicate parameter
        assertNull(QrPairing.parse("https://example.com/pair?v=1&id=$id&t=$token"))
        assertNull(QrPairing.parse("hello"))
    }
}
