import SwiftUI

struct WelcomeView: View {
    @Environment(OnboardingCoordinator.self) private var coordinator
    @Environment(AuthViewModel.self) private var auth

    @State private var name = ""
    @State private var email = ""
    @State private var username = ""
    @State private var password = ""
    @State private var isLoginMode = false

    private var appDisplayName: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleDisplayName") as? String
            ?? Bundle.main.object(forInfoDictionaryKey: "CFBundleName") as? String
            ?? "Spatial"
    }

    private var welcomeTitle: String {
        if isLoginMode { return "Welcome back" }
        return "Welcome to \(appDisplayName)"
    }

    var body: some View {
        ZStack {
            WelcomeHeroBackground()

            ScrollView {
                VStack(spacing: 0) {
                    Spacer(minLength: 56)

                    ZStack {
                        Circle()
                            .fill(Color.white)
                            .frame(width: 107, height: 107)
                        Image("WelcomeLogo")
                            .resizable()
                            .interpolation(.high)
                            .scaledToFit()
                            .frame(width: 62, height: 62)
                    }
                    .padding(.bottom, 20)

                    Text(welcomeTitle)
                        .font(OnboardingStyle.font(24, weight: .bold))
                        .foregroundStyle(.white)
                        .multilineTextAlignment(.center)
                        .shadow(color: .black.opacity(0.35), radius: 6, x: 0, y: 2)
                        .padding(.horizontal, 28)
                        .padding(.bottom, 32)

                    VStack(spacing: 30) {
                        if !isLoginMode {
                            TextField("", text: $name, prompt: Text("Name").foregroundColor(.black.opacity(0.45)))
                                .textContentType(.name)
                                .welcomeSignUpField()
                        }
                        TextField("", text: $email, prompt: Text("Email").foregroundColor(.black.opacity(0.45)))
                            .textContentType(.emailAddress)
                            .keyboardType(.emailAddress)
                            .textInputAutocapitalization(.never)
                            .autocorrectionDisabled()
                            .welcomeSignUpField()
                        if !isLoginMode {
                            TextField("", text: $username, prompt: Text("Username").foregroundColor(.black.opacity(0.45)))
                                .textInputAutocapitalization(.never)
                                .autocorrectionDisabled()
                                .welcomeSignUpField()
                        }
                        SecureField("", text: $password, prompt: Text("Password").foregroundColor(.black.opacity(0.45)))
                            .welcomeSignUpField()
                    }

                    if let err = auth.errorMessage {
                        Text(err)
                            .font(OnboardingStyle.font(12))
                            .foregroundStyle(.red.opacity(0.95))
                            .frame(width: OnboardingStyle.fieldWidth, alignment: .leading)
                            .padding(.top, 16)
                    }

                    Button {
                        Task { await submit() }
                    } label: {
                        if auth.isLoading {
                            ProgressView().tint(.white)
                        } else {
                            Text("Get Started")
                        }
                    }
                    .buttonStyle(WelcomePrimaryButtonStyle())
                    .disabled(auth.isLoading)
                    .padding(.top, auth.errorMessage == nil ? 40 : 24)

                    Button {
                        isLoginMode.toggle()
                    } label: {
                        Text(isLoginMode
                             ? "New here? Create an account"
                             : "Already have an account? Log in")
                            .font(OnboardingStyle.font(13))
                            .foregroundStyle(.white.opacity(0.92))
                            .underline()
                    }
                    .padding(.top, 20)

                    Spacer(minLength: 48)
                }
                .frame(maxWidth: .infinity)
                .padding(.horizontal, 28)
            }
        }
    }

    private func submit() async {
        if isLoginMode {
            await auth.signIn(email: email, password: password)
        } else {
            await auth.signUp(email: email, password: password,
                              username: username, displayName: name)
        }
        if auth.currentSession != nil {
            coordinator.step = .linkWristband
        }
    }
}

#Preview {
    WelcomeView()
        .environment(OnboardingCoordinator())
        .environment(AuthViewModel())
}
