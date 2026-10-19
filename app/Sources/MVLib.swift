import Metal

/// Biblioteca Metal de la app (default.metallib del paquete). El banco de render de CI (bench/) sustituye este fichero por otro que la
/// carga de una ruta, porque un ejecutable de línea de comandos no tiene paquete.
enum MVLib {
    static func library(_ device: MTLDevice) -> MTLLibrary? { device.makeDefaultLibrary() }
}
