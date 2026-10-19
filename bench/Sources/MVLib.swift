import Metal
import Foundation

/// Banco de render: la biblioteca Metal se carga de una ruta (un ejecutable de línea de comandos no tiene paquete con default.metallib).
enum MVLib {
    static var path = ""
    static func library(_ device: MTLDevice) -> MTLLibrary? {
        do { return try device.makeLibrary(URL: URL(fileURLWithPath: path)) } catch { print("MVLib: no se pudo cargar \(path): \(error)"); return nil }
    }
}
