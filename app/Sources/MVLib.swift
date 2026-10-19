import Metal

/// Biblioteca Metal de la app (default.metallib del paquete). El banco de render de CI (bench/) sustituye este fichero por otro que la
/// carga de una ruta, porque un ejecutable de línea de comandos no tiene paquete.
enum MVLib {
    static func library(_ device: MTLDevice) -> MTLLibrary? {
        if let lib = device.makeDefaultLibrary() { return lib }
        // Sin biblioteca no hay pipelines y el inmersivo no puede dibujar nada: queda en el registro.
        Storage.log("MVLib: makeDefaultLibrary() devolvió nil (¿falta default.metallib en \(Bundle.main.bundlePath)?)")
        return nil
    }
}
