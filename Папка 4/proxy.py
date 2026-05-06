import socket
import threading
import re
import sys

class HTTPProxy:
    def __init__(self, host='127.0.0.5', port=8080):
        self.host = host
        self.port = port
        
    def start(self):
        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((self.host, self.port))
        server.listen(50)  # Увеличил очередь
        print(f"[✓] Proxy started on {self.host}:{self.port}", flush=True)
        
        while True:
            try:
                client, addr = server.accept()
                threading.Thread(target=self.handle, args=(client,), daemon=True).start()
            except KeyboardInterrupt:
                print("\n[!] Stopping...")
                break
    
    def handle(self, client):
        try:
            # Читаем запрос от браузера
            request = self.recv_all(client, 4096)
            if not request:
                return
            
            # Парсим первую строку
            try:
                first_line = request.split(b'\r\n')[0].decode()
                method, full_url, version = first_line.split(' ', 2)
            except:
                return
            
            # Парсим URL
            match = re.match(r'http://([^:/]+):?(\d*)(/.*)?', full_url)
            if not match:
                return
            host, port_s, path = match.groups()
            port = int(port_s) if port_s else 80
            path = path or '/'
            
            print(f"[REQ] {method} {full_url}", flush=True)
            
            # Формируем запрос к целевому серверу (относительный путь)
            new_req = f"{method} {path} {version}\r\n"
            for line in request.split(b'\r\n')[1:]:
                if line:
                    txt = line.decode('utf-8', errors='ignore')
                    if txt.lower().startswith('host:'):
                        new_req += f"Host: {host}\r\n"
                    elif txt.lower().startswith('proxy-'):
                        continue  # Удаляем proxy-заголовки
                    else:
                        new_req += txt + '\r\n'
            new_req += '\r\n'
            
            # Подключаемся к целевому серверу
            dest = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            dest.settimeout(10)
            dest.connect((host, port))
            dest.sendall(new_req.encode())
            
            # Читаем и пересылаем ответ ПОТОКОВО
            logged = False
            while True:
                try:
                    chunk = dest.recv(4096)
                    if not chunk:
                        break
                    client.sendall(chunk)
                    
                    # Логируем после получения первой порции с заголовками
                    if not logged and b'\r\n\r\n' in chunk:
                        resp_start = chunk[:chunk.find(b'\r\n\r\n')].decode('utf-8', errors='ignore')
                        status_line = resp_start.split('\r\n')[0]
                        parts = status_line.split(' ')
                        code = parts[1] if len(parts) > 1 else '000'
                        msg = self.status_msg(code)
                        print(f"[LOG] {full_url} → {code} {msg}", flush=True)
                        logged = True
                        
                        # Если это не поток (нет chunked и есть Content-Length), можно оптимизировать
                        if 'Transfer-Encoding: chunked' not in resp_start and 'Content-Length:' in resp_start:
                            pass  # Продолжаем читать до конца
                except socket.timeout:
                    break
                except:
                    break
            
            dest.close()
        except Exception as e:
            print(f"[ERR] {e}", flush=True)
        finally:
            client.close()
    
    def recv_all(self, sock, size):
        """Чтение с обработкой keep-alive"""
        data = b''
        sock.settimeout(5)
        try:
            while True:
                chunk = sock.recv(size)
                if not chunk:
                    break
                data += chunk
                if b'\r\n\r\n' in data:
                    # Проверка Content-Length
                    headers = data[:data.find(b'\r\n\r\n')].decode('utf-8', errors='ignore')
                    if 'Content-Length:' in headers:
                        m = re.search(r'Content-Length:\s*(\d+)', headers)
                        if m:
                            body_start = data.find(b'\r\n\r\n') + 4
                            if len(data) - body_start >= int(m.group(1)):
                                break
                    elif 'Transfer-Encoding: chunked' in headers:
                        # Для chunked читаем пока есть данные
                        if len(chunk) < size:
                            break
                    else:
                        # Нет заголовков длины - считаем запрос завершённым
                        break
        except socket.timeout:
            pass
        return data
    
    def status_msg(self, code):
        return {
            '100': 'Continue', '200': 'OK', '201': 'Created', '204': 'No Content',
            '301': 'Moved', '302': 'Found', '304': 'Not Modified',
            '400': 'Bad Request', '401': 'Unauthorized', '403': 'Forbidden',
            '404': 'Not Found', '500': 'Server Error', '502': 'Bad Gateway',
            '503': 'Unavailable'
        }.get(code, 'Unknown')

if __name__ == "__main__":
    print(f"[*] HTTP Proxy Logger")
    print(f"[*] Listen: 127.0.0.5:8080")
    print(f"[*] Browser proxy: 127.0.0.5:8080")
    print("-" * 40, flush=True)
    HTTPProxy().start()
