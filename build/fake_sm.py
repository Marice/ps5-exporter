from http.server import BaseHTTPRequestHandler, HTTPServer
import json
R = {
 "/api/v1/version": {"status":0,"api_version":1,"shadowmount_version":"1.7","capabilities":["x"]},
 "/api/v1/storage": {"status":0,"mounts":[{"source":"/dev/da0","mount_point":"/mnt/ext1","filesystem":"bfs","total_bytes":1000184545280,"free_bytes":923826782208,"available_bytes":923826782208,"used_bytes":76357763072,"read_only":False},{"source":"/dev/x","mount_point":"/user","filesystem":"ufs","total_bytes":100,"free_bytes":50,"available_bytes":40,"used_bytes":60,"read_only":True}],"destinations":[]},
 "/api/v1/games": {"status":0,"count":2,"size_included":False,"games":[{"path":"/data/homebrew/PPSA01153","title_id":"PPSA01153","title_name":"OliSe \"Player\"","platform":"ps5","source_type":"folder","mounted":True,"installed":True,"source_available":True},{"path":"/mnt/ext1/x.exfat","title_id":"PPSA25646","title_name":"Game, {with} braces","platform":"ps5","source_type":"image","mounted":False,"installed":True,"source_available":False}]},
}
class H(BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0)); self.rfile.read(n)
        body = json.dumps(R.get(self.path, {"status":2,"error":"unknown API route"})).encode()
        self.send_response(200 if self.path in R else 404); self.send_header("Content-Type","application/json"); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
HTTPServer(("127.0.0.1", 10101), H).serve_forever()
