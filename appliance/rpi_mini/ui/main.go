package main

import (
	"fmt"
	"net/http"
	"os/exec"
	"io/ioutil"
)

func hello(w http.ResponseWriter, req *http.Request) {
	out, err := exec.Command("sysctl", "-n", "dev.iicdts.0.temperature").Output()
	if err != nil {
		fmt.Fprintf(w, "Don't know!\n")
		return
	}

	fmt.Fprintf(w, "%s", out)
	fmt.Printf("called...: %s\n", out)
}

func ui(w http.ResponseWriter, req *http.Request) {
	data, err := ioutil.ReadFile("ui.html")
	if err != nil {
		fmt.Fprintf(w, "error!\n")
		return
	}

	w.Header().Set("Content-Length", fmt.Sprintf("%d", len(data)))
	w.Write(data)
	
}

func main() {
	http.HandleFunc("/ui", ui)
	http.HandleFunc("/hello", hello)
	http.ListenAndServe(":8090", nil)
}

