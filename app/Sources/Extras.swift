import Foundation

#if MV_HOST_EXTRAS
/// Punto de enganche de los ficheros opcionales del motor propio (se compilan solo si CI los añade y compilan).
/// Por ahora solo comprueba que enlazan; el uso concreto se cablea aquí.
enum MVExtras {
    static func start() {
        Storage.log("extras opcionales presentes")
    }
}
#endif
