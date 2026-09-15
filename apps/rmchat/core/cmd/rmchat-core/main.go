package main

import (
	"aurora/internal/rmchat"
	"context"
	"flag"
	"fmt"
	"os"
	"os/signal"
)

type roots []string

func (r *roots) String() string         { return "" }
func (r *roots) Set(value string) error { *r = append(*r, value); return nil }
func main() {
	flags := flag.NewFlagSet("rmchat-core", flag.ContinueOnError)
	flags.SetOutput(os.Stderr)
	var uploadRoots roots
	socket := flags.String("socket", "", "Absolute private Unix socket path")
	inspectModels := flags.Bool("enable-model-inspection", false, "Enable the bounded, redacted models.inspect diagnostic RPC")
	flags.Var(&uploadRoots, "upload-root", "Absolute permitted PDF directory; repeatable")
	if flags.Parse(os.Args[1:]) != nil || flags.NArg() != 0 || *socket == "" {
		os.Exit(2)
	}
	client, err := rmchat.NewClient(uploadRoots)
	if err != nil {
		fmt.Fprintln(os.Stderr, "Configuration invalide.")
		os.Exit(2)
	}
	if *inspectModels {
		client.EnableModelInspection()
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	defer stop()
	if err = rmchat.Serve(ctx, *socket, client); err != nil {
		fmt.Fprintln(os.Stderr, "Le core n’a pas pu établir sa connexion locale.")
		os.Exit(1)
	}
}
