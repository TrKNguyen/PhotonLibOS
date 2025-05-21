from http.server import HTTPServer, BaseHTTPRequestHandler
import socketserver

class SimpleHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def do_GET(self):
        self.send_response(200)
        self.send_header('Content-type', 'text/plain')
        self.end_headers()
        self.wfile.write(b"Hello, GET request received")

    def do_POST(self):
        content_length = int(self.headers['Content-Length'])
        post_data = self.rfile.read(content_length)
        self.send_response(200)
        self.send_header('Content-type', 'text/plain')
        self.end_headers()
        self.wfile.write(b"Hello, POST request received: " + post_data)

with socketserver.TCPServer(("", 8080), SimpleHandler) as httpd:
    print("Serving at port 8080")
    httpd.serve_forever()