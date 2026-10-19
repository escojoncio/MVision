import Foundation

/// Sustituto de Storage.log de la app: el banco escribe en la salida estándar.
enum Storage {
    static func log(_ s: String) { print("[app] " + s) }
}
